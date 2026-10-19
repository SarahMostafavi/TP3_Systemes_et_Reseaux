#define FUSE_USE_VERSION 26

#include <fuse_lowlevel.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include "tosfs.h"

#define NB_MAX_INODES 32
/* Nombre de dentry que peut contenir un bloc */
#define DENTRIES_PER_BLOCK (TOSFS_BLOCK_SIZE / sizeof(struct tosfs_dentry))
/* Durée (en secondes) pendant laquelle le noyau garde nos réponses en cache */
#define STAT_TIMEOUT_SEC 1.0
/* Numéro d'inode qui marque une dentry vide */
#define NO_INODE 0

#define min(x, y) ((x) < (y) ? (x) : (y))

/* Zone mémoire de l'image (gardée pour pouvoir la libérer à la fin) */
static char *fs_base;
static size_t fs_size;

/* Structures du filesystem, une fois l'image chargée en mémoire */
static struct tosfs_superblock *superblock;
static struct tosfs_inode *inodes;
static struct tosfs_dentry *root_dentries;

/* Tampon de réponse pour readdir (repris de hello_ll.c) */
struct dirbuf {
    char *p;
    size_t size;
};

/* Charge l'image du filesystem en mémoire avec mmap.
   Renvoie 0 si tout va bien, -1 en cas d'erreur. */
static int map_fs(const char *path)
{
    int fd = open(path, O_RDWR);
    if (fd == -1) {
        perror("open");
        return -1;
    }

    /* fstat donne la taille du fichier, nécessaire pour mmap */
    struct stat st;
    if (fstat(fd, &st) == -1) {
        perror("fstat");
        close(fd);
        return -1;
    }

    fs_size = st.st_size;
    fs_base = mmap(NULL, fs_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);  /* le mapping reste valide après la fermeture */
    if (fs_base == MAP_FAILED) {
        perror("mmap");
        return -1;
    }

    /* Chaque structure commence au début de son bloc */
    superblock = (struct tosfs_superblock *)(fs_base + TOSFS_SUPERBLOCK * TOSFS_BLOCK_SIZE);
    inodes = (struct tosfs_inode *)(fs_base + TOSFS_INODE_BLOCK * TOSFS_BLOCK_SIZE);
    root_dentries = (struct tosfs_dentry *)(fs_base + TOSFS_ROOT_BLOCK * TOSFS_BLOCK_SIZE);
    return 0;
}

/* Écrit les modifications dans le fichier image, puis libère la zone mmap */
static void unmap_fs(void)
{
    if (msync(fs_base, fs_size, MS_SYNC) == -1)
        perror("msync");
    if (munmap(fs_base, fs_size) == -1)
        perror("munmap");
}

static void print_superblock(void)
{
    printf("=== Superblock ===\n");
    printf("magic        : 0x%x (%s)\n", superblock->magic,
           superblock->magic == TOSFS_MAGIC ? "OK" : "INCORRECT");
    printf("block_bitmap : " PRINTF_BINARY_PATTERN_INT32 "\n",
           PRINTF_BYTE_TO_BINARY_INT32(superblock->block_bitmap));
    printf("inode_bitmap : " PRINTF_BINARY_PATTERN_INT32 "\n",
           PRINTF_BYTE_TO_BINARY_INT32(superblock->inode_bitmap));
    printf("block_size   : %u\n", superblock->block_size);
    printf("blocks       : %u\n", superblock->blocks);
    printf("inodes       : %u\n", superblock->inodes);
    printf("root_inode   : %u\n", superblock->root_inode);
}

/* Affiche les inodes utilisées (bit à 1 dans inode_bitmap) */
static void print_inodes(void)
{
    printf("\n=== Inodes ===\n");
    for (int i = 0; i < NB_MAX_INODES; i++) {
        if (!(superblock->inode_bitmap & (1u << i)))
            continue;

        struct tosfs_inode *ino = &inodes[i];
        printf("[%2d] inode=%u block=%u %s perm=%o size=%u uid=%u gid=%u nlink=%u\n",
               i, ino->inode, ino->block_no,
               S_ISDIR(ino->mode) ? "dir " : "file",
               ino->perm, ino->size, ino->uid, ino->gid, ino->nlink);
    }
}

/* Affiche le contenu du dossier racine (ses dentry) */
static void print_root_dir(void)
{
    printf("\n=== Dossier racine ===\n");
    for (size_t i = 0; i < DENTRIES_PER_BLOCK; i++) {
        /* L'inode 0 n'est jamais utilisée : elle marque une case vide */
        if (root_dentries[i].inode == NO_INODE)
            continue;
        printf("inode %u : %s\n", root_dentries[i].inode, root_dentries[i].name);
    }
}

/* Renvoie l'inode associée à ino (le tableau est indexé par numéro d'inode).
   Si ino n'existe pas (bit à 0 dans la bitmap), renvoie NULL. */
static struct tosfs_inode *get_inode(fuse_ino_t ino)
{
    if (ino >= NB_MAX_INODES || !(superblock->inode_bitmap & (1u << ino)))
        return NULL;
    return &inodes[ino];
}

/* Renvoie 1 si ino existe et est un dossier, 0 sinon */
static int is_dir(fuse_ino_t ino)
{
    struct tosfs_inode *inode = get_inode(ino);
    return inode != NULL && S_ISDIR(inode->mode);
}

/* Renvoie l'adresse du bloc de données d'une inode */
static char *inode_data(const struct tosfs_inode *inode)
{
    return fs_base + inode->block_no * TOSFS_BLOCK_SIZE;
}

/* Renvoie le tableau de dentry stocké dans le bloc d'un dossier */
static struct tosfs_dentry *dir_dentries(const struct tosfs_inode *dir)
{
    return (struct tosfs_dentry *)inode_data(dir);
}

/* Cherche name dans le dossier dir.
   Renvoie le numéro d'inode trouvé, ou NO_INODE s'il n'existe pas. */
static fuse_ino_t find_dentry(const struct tosfs_inode *dir, const char *name)
{
    struct tosfs_dentry *dentries = dir_dentries(dir);

    for (size_t i = 0; i < DENTRIES_PER_BLOCK; i++) {
        if (dentries[i].inode == NO_INODE)
            continue;
        /* strncmp : le nom stocké n'a pas de '\0' s'il fait 32 caractères */
        if (strncmp(dentries[i].name, name, TOSFS_MAX_NAME_LENGTH) == 0)
            return dentries[i].inode;
    }
    return NO_INODE;
}

/* Remplit une struct stat à partir d'une inode du filesystem */
static void fill_stat(const struct tosfs_inode *inode, struct stat *st)
{
    memset(st, 0, sizeof(*st));
    st->st_ino     = inode->inode;
    st->st_mode    = inode->mode | inode->perm;  /* type + droits */
    st->st_nlink   = inode->nlink;
    st->st_uid     = inode->uid;
    st->st_gid     = inode->gid;
    st->st_size    = inode->size;
    st->st_blksize = TOSFS_BLOCK_SIZE;
}

/* Ajoute une entrée (nom + inode) au tampon de readdir (repris de hello_ll.c) */
static void dirbuf_add(fuse_req_t req, struct dirbuf *b, const char *name,
                       fuse_ino_t ino)
{
    struct stat stbuf;
    size_t oldsize = b->size;

    /* Premier appel avec NULL : calcule seulement la place nécessaire */
    b->size += fuse_add_direntry(req, NULL, 0, name, NULL, 0);
    b->p = (char *)realloc(b->p, b->size);
    memset(&stbuf, 0, sizeof(stbuf));
    stbuf.st_ino = ino;
    fuse_add_direntry(req, b->p + oldsize, b->size - oldsize, name, &stbuf,
                      b->size);
}

/* Envoie la partie du tampon demandée par FUSE, à partir de off
   et au plus maxsize octets (repris de hello_ll.c) */
static int reply_buf_limited(fuse_req_t req, const char *buf, size_t bufsize,
                             off_t off, size_t maxsize)
{
    if ((size_t)off < bufsize)
        return fuse_reply_buf(req, buf + off, min(bufsize - off, maxsize));
    else
        return fuse_reply_buf(req, NULL, 0);
}

/*  renvoie les attributs d'une inode (stat, ls -l) */
static void futosfs_ll_getattr(fuse_req_t req, fuse_ino_t ino,
                               struct fuse_file_info *fi)
{
    (void)fi;  /* paramètre non utilisé */
    struct tosfs_inode *inode = get_inode(ino);

    if (inode == NULL) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    struct stat st;
    fill_stat(inode, &st);
    fuse_reply_attr(req, &st, STAT_TIMEOUT_SEC);
}

/*  liste le contenu d'un dossier (ls) */
static void futosfs_ll_readdir(fuse_req_t req, fuse_ino_t ino, size_t size,
                               off_t off, struct fuse_file_info *fi)
{
    (void)fi;
    struct tosfs_inode *dir = get_inode(ino);

    if (dir == NULL) {
        fuse_reply_err(req, ENOENT);
        return;
    }
    if (!S_ISDIR(dir->mode)) {
        fuse_reply_err(req, ENOTDIR);
        return;
    }

    struct tosfs_dentry *dentries = dir_dentries(dir);
    struct dirbuf b;
    memset(&b, 0, sizeof(b));

    for (size_t i = 0; i < DENTRIES_PER_BLOCK; i++) {
        if (dentries[i].inode == NO_INODE)
            continue;

        /* Un nom de 32 caractères n'a pas de '\0' final : on en ajoute un */
        char name[TOSFS_MAX_NAME_LENGTH + 1];
        memcpy(name, dentries[i].name, TOSFS_MAX_NAME_LENGTH);
        name[TOSFS_MAX_NAME_LENGTH] = '\0';

        dirbuf_add(req, &b, name, dentries[i].inode);
    }

    reply_buf_limited(req, b.p, b.size, off, size);
    free(b.p);
}

/*  cherche un fichier par son nom dans le dossier parent*/
static void futosfs_ll_lookup(fuse_req_t req, fuse_ino_t parent, const char *name)
{
    if (!is_dir(parent)) {
        fuse_reply_err(req, ENOTDIR);
        return;
    }   
    if (strlen(name) > TOSFS_MAX_NAME_LENGTH) {
        fuse_reply_err(req, ENAMETOOLONG);
        return;
    }

    fuse_ino_t ino = find_dentry(get_inode(parent), name);
    struct tosfs_inode *inode = get_inode(ino);
    if (inode == NULL) {
        fuse_reply_err(req, ENOENT);
        return;
    }

    struct fuse_entry_param e;
    memset(&e, 0, sizeof(e));
    e.ino = ino;
    e.attr_timeout = STAT_TIMEOUT_SEC;
    e.entry_timeout = STAT_TIMEOUT_SEC;
    fill_stat(inode, &e.attr);

    fuse_reply_entry(req, &e);
}

/* ouverture d'un fichier.*/
static void futosfs_ll_open(fuse_req_t req, fuse_ino_t ino,
                            struct fuse_file_info *fi)
{
    struct tosfs_inode *inode = get_inode(ino);

    if (inode == NULL)
        fuse_reply_err(req, ENOENT);
    else if (S_ISDIR(inode->mode))
        fuse_reply_err(req, EISDIR);
    else
        fuse_reply_open(req, fi);
}

/* lit un fichier (cat).
   Envoie au plus size octets du contenu, à partir de l'octet off. */
static void futosfs_ll_read(fuse_req_t req, fuse_ino_t ino, size_t size,
                            off_t off, struct fuse_file_info *fi)
{
    (void)fi; 
    struct tosfs_inode *inode = get_inode(ino);

    if (inode == NULL) {
        fuse_reply_err(req, ENOENT);
        return;
    }
    if (S_ISDIR(inode->mode)) {
        fuse_reply_err(req, EISDIR);
        return;
    }

    /* Seuls les inode->size premiers octets du bloc sont du contenu */
    reply_buf_limited(req, inode_data(inode), inode->size, off, size);
}

/* Fonctions appelées par FUSE */
static struct fuse_lowlevel_ops futosfs_oper = {
    .lookup  = futosfs_ll_lookup,
    .getattr = futosfs_ll_getattr,
    .readdir = futosfs_ll_readdir,
    .open    = futosfs_ll_open,
    .read    = futosfs_ll_read,
};

/* Monte le filesystem et traite les demandes jusqu'au démontage
   (code repris de hello_ll.c). Renvoie 0 si tout s'est bien passé. */
static int run_fuse(int argc, char *argv[])
{
    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    struct fuse_chan *ch;
    char *mountpoint;
    int err = -1;

    if (fuse_parse_cmdline(&args, &mountpoint, NULL, NULL) != -1 &&
        (ch = fuse_mount(mountpoint, &args)) != NULL) {
        struct fuse_session *se;

        se = fuse_lowlevel_new(&args, &futosfs_oper,
                               sizeof(futosfs_oper), NULL);
        if (se != NULL) {
            if (fuse_set_signal_handlers(se) != -1) {
                fuse_session_add_chan(se, ch);
                err = fuse_session_loop(se);
                fuse_remove_signal_handlers(se);
                fuse_session_remove_chan(ch);
            }
            fuse_session_destroy(se);
        }
        fuse_unmount(mountpoint, ch);
    }
    fuse_opt_free_args(&args);

    return err;
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage : %s <image> <point de montage> [options FUSE]\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    if (map_fs(argv[1]) == -1)
        return EXIT_FAILURE;

    print_superblock();
    print_inodes();
    print_root_dir();

   
    argv[1] = argv[0];
    int err = run_fuse(argc - 1, argv + 1);

    unmap_fs();
    return err ? EXIT_FAILURE : EXIT_SUCCESS;
}
