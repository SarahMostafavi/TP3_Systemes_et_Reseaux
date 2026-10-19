#define FUSE_USE_VERSION 26

#include <fuse_lowlevel.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include "tosfs.h"

#define NB_MAX_INODES 32
/* Nombre de dentry que peut contenir un bloc */
#define DENTRIES_PER_BLOCK (TOSFS_BLOCK_SIZE / sizeof(struct tosfs_dentry))

/* Zone mémoire de l'image (gardée pour pouvoir la libérer à la fin) */
static char *fs_base;
static size_t fs_size;

/* Structures du filesystem, une fois l'image chargée en mémoire */
static struct tosfs_superblock *superblock;
static struct tosfs_inode *inodes;
static struct tosfs_dentry *root_dentries;

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
        if (root_dentries[i].inode == 0)
            continue;
        printf("inode %u : %s\n", root_dentries[i].inode, root_dentries[i].name);
    }
}

/* Fonctions appelées par FUSE */
static struct fuse_lowlevel_ops futosfs_oper = {
    
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
