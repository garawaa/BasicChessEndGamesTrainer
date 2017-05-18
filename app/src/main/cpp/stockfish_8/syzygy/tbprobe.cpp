/*
  Copyright (c) 2013 Ronald de Man
  This file may be redistributed and/or modified without restrictions.

  tbprobe.cpp contains the Stockfish-specific routines of the
  tablebase probing code. It should be relatively easy to adapt
  this code to other chess engines.
*/

#define NOMINMAX

#include <algorithm>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <cctype>
#ifndef _WIN32
#include <unistd.h>
#include <sys/mman.h>
#endif

#include "../position.h"
#include "../movegen.h"
#include "../bitboard.h"
#include "../search.h"

#include "tbprobe.h"
#include "tbcore.h"

/////////######### Added from tbcore.cpp by Laurent Bernabe ######////////////////
#define TBMAX_PIECE 254
#define TBMAX_PAWN 256
#define HSHMAX 5

#define Swap(a,b) {int tmp=a;a=b;b=tmp;}

#define TB_PAWN 1
#define TB_KNIGHT 2
#define TB_BISHOP 3
#define TB_ROOK 4
#define TB_QUEEN 5
#define TB_KING 6

#define TB_WPAWN TB_PAWN
#define TB_BPAWN (TB_PAWN | 8)

static LOCK_T TB_mutex;

static bool initialized = false;
static int num_paths = 0;
static char *path_string = NULL;
static char **paths = NULL;

static int TBnum_piece, TBnum_pawn;
static struct TBEntry_piece TB_piece[TBMAX_PIECE];
static struct TBEntry_pawn TB_pawn[TBMAX_PAWN];

static struct TBHashEntry TB_hash[1 << TBHASHBITS][HSHMAX];

#define DTZ_ENTRIES 64

static struct DTZTableEntry DTZ_table[DTZ_ENTRIES];

static void init_indices(void);
uint64 calc_key_from_pcs(int *pcs, int mirror);
static void free_wdl_entry(struct TBEntry *entry);
static void free_dtz_entry(struct TBEntry *entry);

static FD open_tb(const char *str, const char *suffix)
{
    int i;
    FD fd;
    char file[256];

    for (i = 0; i < num_paths; i++) {
        strcpy(file, paths[i]);
        strcat(file, "/");
        strcat(file, str);
        strcat(file, suffix);
#ifndef _WIN32
        fd = open(file, O_RDONLY);
#else
        fd = CreateFile(file, GENERIC_READ, FILE_SHARE_READ, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
#endif
        if (fd != FD_ERR) return fd;
    }
    return FD_ERR;
}

static void close_tb(FD fd)
{
#ifndef _WIN32
    close(fd);
#else
    CloseHandle(fd);
#endif
}

static char *map_file(const char *name, const char *suffix, uint64 *mapping)
{
    FD fd = open_tb(name, suffix);
    if (fd == FD_ERR)
        return NULL;
#ifndef _WIN32
    struct stat statbuf;
    fstat(fd, &statbuf);
    *mapping = statbuf.st_size;
    char *data = (char *)mmap(NULL, statbuf.st_size, PROT_READ,
                              MAP_SHARED, fd, 0);
    if (data == (char *)(-1)) {
        lock_res << "Could not mmap()"<< name << ".\n" << unlock_res;
        // Laurent Bernabe : replace exit by return
        return nullptr;
    }
#else
    DWORD size_low, size_high;
  size_low = GetFileSize(fd, &size_high);
//  *size = ((uint64)size_high) << 32 | ((uint64)size_low);
  HANDLE map = CreateFileMapping(fd, NULL, PAGE_READONLY, size_high, size_low,
                                  NULL);
  if (map == NULL) {
    lock_res << "CreateFileMapping() failed.\n" << unlock_res;
    exit(1);
  }
  *mapping = (uint64)map;
  char *data = (char *)MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0);
  if (data == NULL) {
    lock_res << "MapViewOfFile() failed, name = "<< name << suffix << ", error = "<< GetLastError() <<".\n" << unlock_res;
    // Laurent Bernabe : replace exit by return
    return;
  }
#endif
    close_tb(fd);
    return data;
}

#ifndef _WIN32
static void unmap_file(char *data, uint64 size)
{
    if (!data) return;
    munmap(data, size);
}
#else
static void unmap_file(char *data, uint64 mapping)
{
  if (!data) return;
  UnmapViewOfFile(data);
  CloseHandle((HANDLE)mapping);
}
#endif

static void add_to_hash(struct TBEntry *ptr, uint64 key)
{
    int i, hshidx;

    hshidx = key >> (64 - TBHASHBITS);
    i = 0;
    while (i < HSHMAX && TB_hash[hshidx][i].ptr)
        i++;
    if (i == HSHMAX) {
        lock_res << "HSHMAX too low!\n" << unlock_res;
        // Laurent Bernabe : replace exit by return
        return;
    } else {
        TB_hash[hshidx][i].key = key;
        TB_hash[hshidx][i].ptr = ptr;
    }
}

static char pchr[] = {'K', 'Q', 'R', 'B', 'N', 'P'};

static void init_tb(char *str)
{
    FD fd;
    struct TBEntry *entry;
    int i, j, pcs[16];
    uint64 key, key2;
    int color;
    char *s;

    fd = open_tb(str, WDLSUFFIX);
    if (fd == FD_ERR) {
        return;
    }
    close_tb(fd);

    for (i = 0; i < 16; i++)
        pcs[i] = 0;
    color = 0;
    for (s = str; *s; s++)
        switch (*s) { // Laurent Bernabe : using lowercase
            case 'p':
                pcs[TB_PAWN | color]++;
                break;
            case 'n':
                pcs[TB_KNIGHT | color]++;
                break;
            case 'b':
                pcs[TB_BISHOP | color]++;
                break;
            case 'r':
                pcs[TB_ROOK | color]++;
                break;
            case 'q':
                pcs[TB_QUEEN | color]++;
                break;
            case 'k':
                pcs[TB_KING | color]++;
                break;
            case 'v':
                color = 0x08;
                break;
        }
    for (i = 0; i < 8; i++)
        if (pcs[i] != pcs[i+8])
            break;
    key = calc_key_from_pcs(pcs, 0);
    key2 = calc_key_from_pcs(pcs, 1);
    if (pcs[TB_WPAWN] + pcs[TB_BPAWN] == 0) {
        if (TBnum_piece == TBMAX_PIECE) {
            lock_res << "TBMAX_PIECE limit too low!\n" << unlock_res;
            // Laurent Bernabe : replace exit by return
            return;
        }
        entry = (struct TBEntry *)&TB_piece[TBnum_piece++];
    } else {
        if (TBnum_pawn == TBMAX_PAWN) {
            lock_res << "TBMAX_PAWN limit too low!\n" << unlock_res;
            // Laurent Bernabe : replace exit by return
            return;
        }
        entry = (struct TBEntry *)&TB_pawn[TBnum_pawn++];
    }
    entry->key = key;
    entry->ready = 0;
    entry->num = 0;
    for (i = 0; i < 16; i++)
        entry->num += (ubyte)pcs[i];
    entry->symmetric = (key == key2);
    entry->has_pawns = (pcs[TB_WPAWN] + pcs[TB_BPAWN] > 0);
    if (entry->num > Tablebases::MaxCardinality)
        Tablebases::MaxCardinality = entry->num;

    if (entry->has_pawns) {
        struct TBEntry_pawn *ptr = (struct TBEntry_pawn *)entry;
        ptr->pawns[0] = (ubyte)pcs[TB_WPAWN];
        ptr->pawns[1] = (ubyte)pcs[TB_BPAWN];
        if (pcs[TB_BPAWN] > 0
            && (pcs[TB_WPAWN] == 0 || pcs[TB_BPAWN] < pcs[TB_WPAWN])) {
            ptr->pawns[0] = (ubyte)pcs[TB_BPAWN];
            ptr->pawns[1] = (ubyte)pcs[TB_WPAWN];
        }
    } else {
        struct TBEntry_piece *ptr = (struct TBEntry_piece *)entry;
        for (i = 0, j = 0; i < 16; i++)
            if (pcs[i] == 1) j++;
        if (j >= 3) ptr->enc_type = 0;
        else if (j == 2) ptr->enc_type = 2;
        else { /* only for suicide */
            j = 16;
            for (i = 0; i < 16; i++) {
                if (pcs[i] < j && pcs[i] > 1) j = pcs[i];
                ptr->enc_type = ubyte(1 + j);
            }
        }
    }
    add_to_hash(entry, key);
    if (key2 != key) add_to_hash(entry, key2);
}

void Tablebases::init(const std::string& path)
{
    char str[16];
    int i, j, k, l;

    if (initialized) {
        free(path_string);
        free(paths);
        struct TBEntry *entry;
        for (i = 0; i < TBnum_piece; i++) {
            entry = (struct TBEntry *)&TB_piece[i];
            free_wdl_entry(entry);
        }
        for (i = 0; i < TBnum_pawn; i++) {
            entry = (struct TBEntry *)&TB_pawn[i];
            free_wdl_entry(entry);
        }
        for (i = 0; i < DTZ_ENTRIES; i++)
            if (DTZ_table[i].entry)
                free_dtz_entry(DTZ_table[i].entry);
    } else {
        init_indices();
        initialized = true;
    }

    const char *p = path.c_str();
    if (strlen(p) == 0 || !strcmp(p, "<empty>")) return;
    path_string = (char *)malloc(strlen(p) + 1);
    strcpy(path_string, p);
    num_paths = 0;
    for (i = 0;; i++) {
        if (path_string[i] != SEP_CHAR)
            num_paths++;
        while (path_string[i] && path_string[i] != SEP_CHAR)
            i++;
        if (!path_string[i]) break;
        path_string[i] = 0;
    }
    paths = (char **)malloc(num_paths * sizeof(char *));
    for (i = j = 0; i < num_paths; i++) {
        while (!path_string[j]) j++;
        paths[i] = &path_string[j];
        while (path_string[j]) j++;
    }

    LOCK_INIT(TB_mutex);

    TBnum_piece = TBnum_pawn = 0;
    MaxCardinality = 0;

    for (i = 0; i < (1 << TBHASHBITS); i++)
        for (j = 0; j < HSHMAX; j++) {
            TB_hash[i][j].key = 0ULL;
            TB_hash[i][j].ptr = NULL;
        }

    for (i = 0; i < DTZ_ENTRIES; i++)
        DTZ_table[i].entry = NULL;

    for (i = 1; i < 6; i++) {
        sprintf(str, "k%cvk", tolower(pchr[i])); // Laurent Bernabe : using lowercase
        init_tb(str);
    }

    for (i = 1; i < 6; i++)
        for (j = i; j < 6; j++) {
            sprintf(str, "k%cvk%c", tolower(pchr[i]), tolower(pchr[j])); // Laurent Bernabe : using lowercase
            init_tb(str);
        }

    for (i = 1; i < 6; i++)
        for (j = i; j < 6; j++) {
            sprintf(str, "k%c%cvk", tolower(pchr[i]), tolower(pchr[j])); // Laurent Bernabe : using lowercase
            init_tb(str);
        }

    for (i = 1; i < 6; i++)
        for (j = i; j < 6; j++)
            for (k = 1; k < 6; k++) {
                sprintf(str, "k%c%cvk%c", tolower(pchr[i]), tolower(pchr[j]), tolower(pchr[k])); // Laurent Bernabe : using lowercase
                init_tb(str);
            }

    for (i = 1; i < 6; i++)
        for (j = i; j < 6; j++)
            for (k = j; k < 6; k++) {
                sprintf(str, "k%c%c%cvk", tolower(pchr[i]), tolower(pchr[j]), tolower(pchr[k]));  // Laurent Bernabe : using lowercase
                init_tb(str);
            }

    for (i = 1; i < 6; i++)
        for (j = i; j < 6; j++)
            for (k = i; k < 6; k++)
                for (l = (i == k) ? j : k; l < 6; l++) {
                    sprintf(str, "k%c%cvk%c%c", tolower(pchr[i]), tolower(pchr[j]), tolower(pchr[k]), tolower(pchr[l]));  // Laurent Bernabe : using lowercase
                    init_tb(str);
                }

    for (i = 1; i < 6; i++)
        for (j = i; j < 6; j++)
            for (k = j; k < 6; k++)
                for (l = 1; l < 6; l++) {
                    sprintf(str, "k%c%c%cvk%c", tolower(pchr[i]), tolower(pchr[j]), tolower(pchr[k]), tolower(pchr[l]));  // Laurent Bernabe : using lowercase
                    init_tb(str);
                }

    for (i = 1; i < 6; i++)
        for (j = i; j < 6; j++)
            for (k = j; k < 6; k++)
                for (l = k; l < 6; l++) {
                    sprintf(str, "k%c%c%c%cvk", tolower(pchr[i]), tolower(pchr[j]), tolower(pchr[k]), tolower(pchr[l]));  // Laurent Bernabe : using lowercase
                    init_tb(str);
                }

    lock_res << "info string Found " << TBnum_piece + TBnum_pawn << " tablebases.\n" << unlock_res;
}

static const signed char offdiag[] = {
        0,-1,-1,-1,-1,-1,-1,-1,
        1, 0,-1,-1,-1,-1,-1,-1,
        1, 1, 0,-1,-1,-1,-1,-1,
        1, 1, 1, 0,-1,-1,-1,-1,
        1, 1, 1, 1, 0,-1,-1,-1,
        1, 1, 1, 1, 1, 0,-1,-1,
        1, 1, 1, 1, 1, 1, 0,-1,
        1, 1, 1, 1, 1, 1, 1, 0
};

static const ubyte triangle[] = {
        6, 0, 1, 2, 2, 1, 0, 6,
        0, 7, 3, 4, 4, 3, 7, 0,
        1, 3, 8, 5, 5, 8, 3, 1,
        2, 4, 5, 9, 9, 5, 4, 2,
        2, 4, 5, 9, 9, 5, 4, 2,
        1, 3, 8, 5, 5, 8, 3, 1,
        0, 7, 3, 4, 4, 3, 7, 0,
        6, 0, 1, 2, 2, 1, 0, 6
};

static const ubyte invtriangle[] = {
        1, 2, 3, 10, 11, 19, 0, 9, 18, 27
};

static const ubyte invdiag[] = {
        0, 9, 18, 27, 36, 45, 54, 63,
        7, 14, 21, 28, 35, 42, 49, 56
};

static const ubyte flipdiag[] = {
        0,  8, 16, 24, 32, 40, 48, 56,
        1,  9, 17, 25, 33, 41, 49, 57,
        2, 10, 18, 26, 34, 42, 50, 58,
        3, 11, 19, 27, 35, 43, 51, 59,
        4, 12, 20, 28, 36, 44, 52, 60,
        5, 13, 21, 29, 37, 45, 53, 61,
        6, 14, 22, 30, 38, 46, 54, 62,
        7, 15, 23, 31, 39, 47, 55, 63
};

static const ubyte lower[] = {
        28,  0,  1,  2,  3,  4,  5,  6,
        0, 29,  7,  8,  9, 10, 11, 12,
        1,  7, 30, 13, 14, 15, 16, 17,
        2,  8, 13, 31, 18, 19, 20, 21,
        3,  9, 14, 18, 32, 22, 23, 24,
        4, 10, 15, 19, 22, 33, 25, 26,
        5, 11, 16, 20, 23, 25, 34, 27,
        6, 12, 17, 21, 24, 26, 27, 35
};

static const ubyte diag[] = {
        0,  0,  0,  0,  0,  0,  0,  8,
        0,  1,  0,  0,  0,  0,  9,  0,
        0,  0,  2,  0,  0, 10,  0,  0,
        0,  0,  0,  3, 11,  0,  0,  0,
        0,  0,  0, 12,  4,  0,  0,  0,
        0,  0, 13,  0,  0,  5,  0,  0,
        0, 14,  0,  0,  0,  0,  6,  0,
        15,  0,  0,  0,  0,  0,  0,  7
};

static const ubyte flap[] = {
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 6, 12, 18, 18, 12, 6, 0,
        1, 7, 13, 19, 19, 13, 7, 1,
        2, 8, 14, 20, 20, 14, 8, 2,
        3, 9, 15, 21, 21, 15, 9, 3,
        4, 10, 16, 22, 22, 16, 10, 4,
        5, 11, 17, 23, 23, 17, 11, 5,
        0, 0, 0, 0, 0, 0, 0, 0
};

static const ubyte ptwist[] = {
        0, 0, 0, 0, 0, 0, 0, 0,
        47, 35, 23, 11, 10, 22, 34, 46,
        45, 33, 21, 9, 8, 20, 32, 44,
        43, 31, 19, 7, 6, 18, 30, 42,
        41, 29, 17, 5, 4, 16, 28, 40,
        39, 27, 15, 3, 2, 14, 26, 38,
        37, 25, 13, 1, 0, 12, 24, 36,
        0, 0, 0, 0, 0, 0, 0, 0
};

static const ubyte invflap[] = {
        8, 16, 24, 32, 40, 48,
        9, 17, 25, 33, 41, 49,
        10, 18, 26, 34, 42, 50,
        11, 19, 27, 35, 43, 51
};

static const ubyte invptwist[] = {
        52, 51, 44, 43, 36, 35, 28, 27, 20, 19, 12, 11,
        53, 50, 45, 42, 37, 34, 29, 26, 21, 18, 13, 10,
        54, 49, 46, 41, 38, 33, 30, 25, 22, 17, 14, 9,
        55, 48, 47, 40, 39, 32, 31, 24, 23, 16, 15, 8
};

static const ubyte file_to_file[] = {
        0, 1, 2, 3, 3, 2, 1, 0
};

static const short KK_idx[10][64] = {
        { -1, -1, -1,  0,  1,  2,  3,  4,
                -1, -1, -1,  5,  6,  7,  8,  9,
                10, 11, 12, 13, 14, 15, 16, 17,
                18, 19, 20, 21, 22, 23, 24, 25,
                26, 27, 28, 29, 30, 31, 32, 33,
                34, 35, 36, 37, 38, 39, 40, 41,
                42, 43, 44, 45, 46, 47, 48, 49,
                50, 51, 52, 53, 54, 55, 56, 57 },
        { 58, -1, -1, -1, 59, 60, 61, 62,
                63, -1, -1, -1, 64, 65, 66, 67,
                68, 69, 70, 71, 72, 73, 74, 75,
                76, 77, 78, 79, 80, 81, 82, 83,
                84, 85, 86, 87, 88, 89, 90, 91,
                92, 93, 94, 95, 96, 97, 98, 99,
                100,101,102,103,104,105,106,107,
                108,109,110,111,112,113,114,115},
        {116,117, -1, -1, -1,118,119,120,
                121,122, -1, -1, -1,123,124,125,
                126,127,128,129,130,131,132,133,
                134,135,136,137,138,139,140,141,
                142,143,144,145,146,147,148,149,
                150,151,152,153,154,155,156,157,
                158,159,160,161,162,163,164,165,
                166,167,168,169,170,171,172,173 },
        {174, -1, -1, -1,175,176,177,178,
                179, -1, -1, -1,180,181,182,183,
                184, -1, -1, -1,185,186,187,188,
                189,190,191,192,193,194,195,196,
                197,198,199,200,201,202,203,204,
                205,206,207,208,209,210,211,212,
                213,214,215,216,217,218,219,220,
                221,222,223,224,225,226,227,228 },
        {229,230, -1, -1, -1,231,232,233,
                234,235, -1, -1, -1,236,237,238,
                239,240, -1, -1, -1,241,242,243,
                244,245,246,247,248,249,250,251,
                252,253,254,255,256,257,258,259,
                260,261,262,263,264,265,266,267,
                268,269,270,271,272,273,274,275,
                276,277,278,279,280,281,282,283 },
        {284,285,286,287,288,289,290,291,
                292,293, -1, -1, -1,294,295,296,
                297,298, -1, -1, -1,299,300,301,
                302,303, -1, -1, -1,304,305,306,
                307,308,309,310,311,312,313,314,
                315,316,317,318,319,320,321,322,
                323,324,325,326,327,328,329,330,
                331,332,333,334,335,336,337,338 },
        { -1, -1,339,340,341,342,343,344,
                -1, -1,345,346,347,348,349,350,
                -1, -1,441,351,352,353,354,355,
                -1, -1, -1,442,356,357,358,359,
                -1, -1, -1, -1,443,360,361,362,
                -1, -1, -1, -1, -1,444,363,364,
                -1, -1, -1, -1, -1, -1,445,365,
                -1, -1, -1, -1, -1, -1, -1,446 },
        { -1, -1, -1,366,367,368,369,370,
                -1, -1, -1,371,372,373,374,375,
                -1, -1, -1,376,377,378,379,380,
                -1, -1, -1,447,381,382,383,384,
                -1, -1, -1, -1,448,385,386,387,
                -1, -1, -1, -1, -1,449,388,389,
                -1, -1, -1, -1, -1, -1,450,390,
                -1, -1, -1, -1, -1, -1, -1,451 },
        {452,391,392,393,394,395,396,397,
                -1, -1, -1, -1,398,399,400,401,
                -1, -1, -1, -1,402,403,404,405,
                -1, -1, -1, -1,406,407,408,409,
                -1, -1, -1, -1,453,410,411,412,
                -1, -1, -1, -1, -1,454,413,414,
                -1, -1, -1, -1, -1, -1,455,415,
                -1, -1, -1, -1, -1, -1, -1,456 },
        {457,416,417,418,419,420,421,422,
                -1,458,423,424,425,426,427,428,
                -1, -1, -1, -1, -1,429,430,431,
                -1, -1, -1, -1, -1,432,433,434,
                -1, -1, -1, -1, -1,435,436,437,
                -1, -1, -1, -1, -1,459,438,439,
                -1, -1, -1, -1, -1, -1,460,440,
                -1, -1, -1, -1, -1, -1, -1,461 }
};

static int binomial[5][64];
static int pawnidx[5][24];
static int pfactor[5][4];

static void init_indices(void)
{
    int i, j, k;

// binomial[k-1][n] = Bin(n, k)
    for (i = 0; i < 5; i++)
        for (j = 0; j < 64; j++) {
            int f = j;
            int l = 1;
            for (k = 1; k <= i; k++) {
                f *= (j - k);
                l *= (k + 1);
            }
            binomial[i][j] = f / l;
        }

    for (i = 0; i < 5; i++) {
        int s = 0;
        for (j = 0; j < 6; j++) {
            pawnidx[i][j] = s;
            s += (i == 0) ? 1 : binomial[i - 1][ptwist[invflap[j]]];
        }
        pfactor[i][0] = s;
        s = 0;
        for (; j < 12; j++) {
            pawnidx[i][j] = s;
            s += (i == 0) ? 1 : binomial[i - 1][ptwist[invflap[j]]];
        }
        pfactor[i][1] = s;
        s = 0;
        for (; j < 18; j++) {
            pawnidx[i][j] = s;
            s += (i == 0) ? 1 : binomial[i - 1][ptwist[invflap[j]]];
        }
        pfactor[i][2] = s;
        s = 0;
        for (; j < 24; j++) {
            pawnidx[i][j] = s;
            s += (i == 0) ? 1 : binomial[i - 1][ptwist[invflap[j]]];
        }
        pfactor[i][3] = s;
    }
}

static uint64 encode_piece(struct TBEntry_piece *ptr, ubyte *norm, int *pos, int *factor)
{
    uint64 idx;
    int i, j, k, m, l, p;
    int n = ptr->num;

    if (pos[0] & 0x04) {
        for (i = 0; i < n; i++)
            pos[i] ^= 0x07;
    }
    if (pos[0] & 0x20) {
        for (i = 0; i < n; i++)
            pos[i] ^= 0x38;
    }

    for (i = 0; i < n; i++)
        if (offdiag[pos[i]]) break;
    if (i < (ptr->enc_type == 0 ? 3 : 2) && offdiag[pos[i]] > 0)
        for (i = 0; i < n; i++)
            pos[i] = flipdiag[pos[i]];

    switch (ptr->enc_type) {

        case 0: /* 111 */
            i = (pos[1] > pos[0]);
            j = (pos[2] > pos[0]) + (pos[2] > pos[1]);

            if (offdiag[pos[0]])
                idx = triangle[pos[0]] * 63*62 + (pos[1] - i) * 62 + (pos[2] - j);
            else if (offdiag[pos[1]])
                idx = 6*63*62 + diag[pos[0]] * 28*62 + lower[pos[1]] * 62 + pos[2] - j;
            else if (offdiag[pos[2]])
                idx = 6*63*62 + 4*28*62 + (diag[pos[0]]) * 7*28 + (diag[pos[1]] - i) * 28 + lower[pos[2]];
            else
                idx = 6*63*62 + 4*28*62 + 4*7*28 + (diag[pos[0]] * 7*6) + (diag[pos[1]] - i) * 6 + (diag[pos[2]] - j);
            i = 3;
            break;

        case 1: /* K3 */
            j = (pos[2] > pos[0]) + (pos[2] > pos[1]);

            idx = KK_idx[triangle[pos[0]]][pos[1]];
            if (idx < 441)
                idx = idx + 441 * (pos[2] - j);
            else {
                idx = 441*62 + (idx - 441) + 21 * lower[pos[2]];
                if (!offdiag[pos[2]])
                    idx -= j * 21;
            }
            i = 3;
            break;

        default: /* K2 */
            idx = KK_idx[triangle[pos[0]]][pos[1]];
            i = 2;
            break;
    }
    idx *= factor[0];

    for (; i < n;) {
        int t = norm[i];
        for (j = i; j < i + t; j++)
            for (k = j + 1; k < i + t; k++)
                if (pos[j] > pos[k]) Swap(pos[j], pos[k]);
        int s = 0;
        for (m = i; m < i + t; m++) {
            p = pos[m];
            for (l = 0, j = 0; l < i; l++)
                j += (p > pos[l]);
            s += binomial[m - i][p - j];
        }
        idx += ((uint64)s) * ((uint64)factor[i]);
        i += t;
    }

    return idx;
}

// determine file of leftmost pawn and sort pawns
static int pawn_file(struct TBEntry_pawn *ptr, int *pos)
{
    int i;

    for (i = 1; i < ptr->pawns[0]; i++)
        if (flap[pos[0]] > flap[pos[i]])
        Swap(pos[0], pos[i]);

    return file_to_file[pos[0] & 0x07];
}

static uint64 encode_pawn(struct TBEntry_pawn *ptr, ubyte *norm, int *pos, int *factor)
{
    uint64 idx;
    int i, j, k, m, s, t;
    int n = ptr->num;

    if (pos[0] & 0x04)
        for (i = 0; i < n; i++)
            pos[i] ^= 0x07;

    for (i = 1; i < ptr->pawns[0]; i++)
        for (j = i + 1; j < ptr->pawns[0]; j++)
            if (ptwist[pos[i]] < ptwist[pos[j]])
            Swap(pos[i], pos[j]);

    t = ptr->pawns[0] - 1;
    idx = pawnidx[t][flap[pos[0]]];
    for (i = t; i > 0; i--)
        idx += binomial[t - i][ptwist[pos[i]]];
    idx *= factor[0];

// remaining pawns
    i = ptr->pawns[0];
    t = i + ptr->pawns[1];
    if (t > i) {
        for (j = i; j < t; j++)
            for (k = j + 1; k < t; k++)
                if (pos[j] > pos[k]) Swap(pos[j], pos[k]);
        s = 0;
        for (m = i; m < t; m++) {
            int p = pos[m];
            for (k = 0, j = 0; k < i; k++)
                j += (p > pos[k]);
            s += binomial[m - i][p - j - 8];
        }
        idx += ((uint64)s) * ((uint64)factor[i]);
        i = t;
    }

    for (; i < n;) {
        t = norm[i];
        for (j = i; j < i + t; j++)
            for (k = j + 1; k < i + t; k++)
                if (pos[j] > pos[k]) Swap(pos[j], pos[k]);
        s = 0;
        for (m = i; m < i + t; m++) {
            int p = pos[m];
            for (k = 0, j = 0; k < i; k++)
                j += (p > pos[k]);
            s += binomial[m - i][p - j];
        }
        idx += ((uint64)s) * ((uint64)factor[i]);
        i += t;
    }

    return idx;
}

// place k like pieces on n squares
static int subfactor(int k, int n)
{
    int i, f, l;

    f = n;
    l = 1;
    for (i = 1; i < k; i++) {
        f *= n - i;
        l *= i + 1;
    }

    return f / l;
}

static uint64 calc_factors_piece(int *factor, int num, int order, ubyte *norm, ubyte enc_type)
{
    int i, k, n;
    uint64 f;
    static int pivfac[] = { 31332, 28056, 462 };

    n = 64 - norm[0];

    f = 1;
    for (i = norm[0], k = 0; i < num || k == order; k++) {
        if (k == order) {
            factor[0] = static_cast<int>(f);
            f *= pivfac[enc_type];
        } else {
            factor[i] = static_cast<int>(f);
            f *= subfactor(norm[i], n);
            n -= norm[i];
            i += norm[i];
        }
    }

    return f;
}

static uint64 calc_factors_pawn(int *factor, int num, int order, int order2, ubyte *norm, int file)
{
    int i, k, n;
    uint64 f;

    i = norm[0];
    if (order2 < 0x0f) i += norm[i];
    n = 64 - i;

    f = 1;
    for (k = 0; i < num || k == order || k == order2; k++) {
        if (k == order) {
            factor[0] = static_cast<int>(f);
            f *= pfactor[norm[0] - 1][file];
        } else if (k == order2) {
            factor[norm[0]] = static_cast<int>(f);
            f *= subfactor(norm[norm[0]], 48 - norm[0]);
        } else {
            factor[i] = static_cast<int>(f);
            f *= subfactor(norm[i], n);
            n -= norm[i];
            i += norm[i];
        }
    }

    return f;
}

static void set_norm_piece(struct TBEntry_piece *ptr, ubyte *norm, ubyte *pieces)
{
    int i, j;

    for (i = 0; i < ptr->num; i++)
        norm[i] = 0;

    switch (ptr->enc_type) {
        case 0:
            norm[0] = 3;
            break;
        case 2:
            norm[0] = 2;
            break;
        default:
            norm[0] = ubyte(ptr->enc_type - 1);
            break;
    }

    for (i = norm[0]; i < ptr->num; i += norm[i])
        for (j = i; j < ptr->num && pieces[j] == pieces[i]; j++)
            norm[i]++;
}

static void set_norm_pawn(struct TBEntry_pawn *ptr, ubyte *norm, ubyte *pieces)
{
    int i, j;

    for (i = 0; i < ptr->num; i++)
        norm[i] = 0;

    norm[0] = ptr->pawns[0];
    if (ptr->pawns[1]) norm[ptr->pawns[0]] = ptr->pawns[1];

    for (i = ptr->pawns[0] + ptr->pawns[1]; i < ptr->num; i += norm[i])
        for (j = i; j < ptr->num && pieces[j] == pieces[i]; j++)
            norm[i]++;
}

static void setup_pieces_piece(struct TBEntry_piece *ptr, unsigned char *data, uint64 *tb_size)
{
    int i;
    int order;

    for (i = 0; i < ptr->num; i++)
        ptr->pieces[0][i] = ubyte(data[i + 1] & 0x0f);
    order = data[0] & 0x0f;
    set_norm_piece(ptr, ptr->norm[0], ptr->pieces[0]);
    tb_size[0] = calc_factors_piece(ptr->factor[0], ptr->num, order, ptr->norm[0], ptr->enc_type);

    for (i = 0; i < ptr->num; i++)
        ptr->pieces[1][i] = ubyte(data[i + 1] >> 4);
    order = data[0] >> 4;
    set_norm_piece(ptr, ptr->norm[1], ptr->pieces[1]);
    tb_size[1] = calc_factors_piece(ptr->factor[1], ptr->num, order, ptr->norm[1], ptr->enc_type);
}

static void setup_pieces_piece_dtz(struct DTZEntry_piece *ptr, unsigned char *data, uint64 *tb_size)
{
    int i;
    int order;

    for (i = 0; i < ptr->num; i++)
        ptr->pieces[i] = ubyte(data[i + 1] & 0x0f);
    order = data[0] & 0x0f;
    set_norm_piece((struct TBEntry_piece *)ptr, ptr->norm, ptr->pieces);
    tb_size[0] = calc_factors_piece(ptr->factor, ptr->num, order, ptr->norm, ptr->enc_type);
}

static void setup_pieces_pawn(struct TBEntry_pawn *ptr, unsigned char *data, uint64 *tb_size, int f)
{
    int i, j;
    int order, order2;

    j = 1 + (ptr->pawns[1] > 0);
    order = data[0] & 0x0f;
    order2 = ptr->pawns[1] ? (data[1] & 0x0f) : 0x0f;
    for (i = 0; i < ptr->num; i++)
        ptr->file[f].pieces[0][i] = ubyte(data[i + j] & 0x0f);
    set_norm_pawn(ptr, ptr->file[f].norm[0], ptr->file[f].pieces[0]);
    tb_size[0] = calc_factors_pawn(ptr->file[f].factor[0], ptr->num, order, order2, ptr->file[f].norm[0], f);

    order = data[0] >> 4;
    order2 = ptr->pawns[1] ? (data[1] >> 4) : 0x0f;
    for (i = 0; i < ptr->num; i++)
        ptr->file[f].pieces[1][i] = ubyte(data[i + j] >> 4);
    set_norm_pawn(ptr, ptr->file[f].norm[1], ptr->file[f].pieces[1]);
    tb_size[1] = calc_factors_pawn(ptr->file[f].factor[1], ptr->num, order, order2, ptr->file[f].norm[1], f);
}

static void setup_pieces_pawn_dtz(struct DTZEntry_pawn *ptr, unsigned char *data, uint64 *tb_size, int f)
{
    int i, j;
    int order, order2;

    j = 1 + (ptr->pawns[1] > 0);
    order = data[0] & 0x0f;
    order2 = ptr->pawns[1] ? (data[1] & 0x0f) : 0x0f;
    for (i = 0; i < ptr->num; i++)
        ptr->file[f].pieces[i] = ubyte(data[i + j] & 0x0f);
    set_norm_pawn((struct TBEntry_pawn *)ptr, ptr->file[f].norm, ptr->file[f].pieces);
    tb_size[0] = calc_factors_pawn(ptr->file[f].factor, ptr->num, order, order2, ptr->file[f].norm, f);
}

static void calc_symlen(struct PairsData *d, int s, char *tmp)
{
    int s1, s2;

    ubyte* w = d->sympat + 3 * s;
    s2 = (w[2] << 4) | (w[1] >> 4);
    if (s2 == 0x0fff)
        d->symlen[s] = 0;
    else {
        s1 = ((w[1] & 0xf) << 8) | w[0];
        if (!tmp[s1]) calc_symlen(d, s1, tmp);
        if (!tmp[s2]) calc_symlen(d, s2, tmp);
        d->symlen[s] = ubyte(d->symlen[s1] + d->symlen[s2] + 1);
    }
    tmp[s] = 1;
}

ushort ReadUshort(ubyte* d) {
    return ushort(d[0] | (d[1] << 8));
}

uint32 ReadUint32(ubyte* d) {
    return d[0] | (d[1] << 8) | (d[2] << 16) | (d[3] << 24);
}

static struct PairsData *setup_pairs(unsigned char *data, uint64 tb_size, uint64 *size, unsigned char **next, ubyte *flags, int wdl)
{
    struct PairsData *d;
    int i;

    *flags = data[0];
    if (data[0] & 0x80) {
        d = (struct PairsData *)malloc(sizeof(struct PairsData));
        d->idxbits = 0;
        if (wdl)
            d->min_len = data[1];
        else
            d->min_len = 0;
        *next = data + 2;
        size[0] = size[1] = size[2] = 0;
        return d;
    }

    int blocksize = data[1];
    int idxbits = data[2];
    int real_num_blocks = ReadUint32(&data[4]);
    int num_blocks = real_num_blocks + *(ubyte *)(&data[3]);
    int max_len = data[8];
    int min_len = data[9];
    int h = max_len - min_len + 1;
    int num_syms = ReadUshort(&data[10 + 2 * h]);
    d = (struct PairsData *)malloc(sizeof(struct PairsData) + (h - 1) * sizeof(base_t) + num_syms);
    d->blocksize = blocksize;
    d->idxbits = idxbits;
    d->offset = (ushort*)(&data[10]);
    d->symlen = ((ubyte *)d) + sizeof(struct PairsData) + (h - 1) * sizeof(base_t);
    d->sympat = &data[12 + 2 * h];
    d->min_len = min_len;
    *next = &data[12 + 2 * h + 3 * num_syms + (num_syms & 1)];

    uint64 num_indices = (tb_size + (1ULL << idxbits) - 1) >> idxbits;
    size[0] = 6ULL * num_indices;
    size[1] = 2ULL * num_blocks;
    size[2] = (1ULL << blocksize) * real_num_blocks;

    // char tmp[num_syms];
    char tmp[4096];
    for (i = 0; i < num_syms; i++)
        tmp[i] = 0;
    for (i = 0; i < num_syms; i++)
        if (!tmp[i])
            calc_symlen(d, i, tmp);

    d->base[h - 1] = 0;
    for (i = h - 2; i >= 0; i--)
        d->base[i] = (d->base[i + 1] + ReadUshort((ubyte*)(d->offset + i)) - ReadUshort((ubyte*)(d->offset + i + 1))) / 2;
    for (i = 0; i < h; i++)
        d->base[i] <<= 64 - (min_len + i);

    d->offset -= d->min_len;

    return d;
}

static int init_table_wdl(struct TBEntry *entry, char *str)
{
    ubyte *next;
    int f, s;
    uint64 tb_size[8];
    uint64 size[8 * 3];
    ubyte flags;

    // first mmap the table into memory

    entry->data = map_file(str, WDLSUFFIX, &entry->mapping);
    if (!entry->data) {
        lock_res << "Could not find " << str << WDLSUFFIX << unlock_res;
        return 0;
    }

    ubyte *data = (ubyte *)entry->data;
    if (data[0] != WDL_MAGIC[0] ||
        data[1] != WDL_MAGIC[1] ||
        data[2] != WDL_MAGIC[2] ||
        data[3] != WDL_MAGIC[3]) {
        lock_res << "Corrupted table.\n" << unlock_res;
        unmap_file(entry->data, entry->mapping);
        entry->data = 0;
        return 0;
    }

    int split = data[4] & 0x01;
    int files = data[4] & 0x02 ? 4 : 1;

    data += 5;

    if (!entry->has_pawns) {
        struct TBEntry_piece *ptr = (struct TBEntry_piece *)entry;
        setup_pieces_piece(ptr, data, &tb_size[0]);
        data += ptr->num + 1;
        data += ((uintptr_t)data) & 0x01;

        ptr->precomp[0] = setup_pairs(data, tb_size[0], &size[0], &next, &flags, 1);
        data = next;
        if (split) {
            ptr->precomp[1] = setup_pairs(data, tb_size[1], &size[3], &next, &flags, 1);
            data = next;
        } else
            ptr->precomp[1] = NULL;

        ptr->precomp[0]->indextable = (char *)data;
        data += size[0];
        if (split) {
            ptr->precomp[1]->indextable = (char *)data;
            data += size[3];
        }

        ptr->precomp[0]->sizetable = (ushort *)data;
        data += size[1];
        if (split) {
            ptr->precomp[1]->sizetable = (ushort *)data;
            data += size[4];
        }

        data = (ubyte *)((((uintptr_t)data) + 0x3f) & ~0x3f);
        ptr->precomp[0]->data = data;
        data += size[2];
        if (split) {
            data = (ubyte *)((((uintptr_t)data) + 0x3f) & ~0x3f);
            ptr->precomp[1]->data = data;
        }
    } else {
        struct TBEntry_pawn *ptr = (struct TBEntry_pawn *)entry;
        s = 1 + (ptr->pawns[1] > 0);
        for (f = 0; f < 4; f++) {
            setup_pieces_pawn((struct TBEntry_pawn *)ptr, data, &tb_size[2 * f], f);
            data += ptr->num + s;
        }
        data += ((uintptr_t)data) & 0x01;

        for (f = 0; f < files; f++) {
            ptr->file[f].precomp[0] = setup_pairs(data, tb_size[2 * f], &size[6 * f], &next, &flags, 1);
            data = next;
            if (split) {
                ptr->file[f].precomp[1] = setup_pairs(data, tb_size[2 * f + 1], &size[6 * f + 3], &next, &flags, 1);
                data = next;
            } else
                ptr->file[f].precomp[1] = NULL;
        }

        for (f = 0; f < files; f++) {
            ptr->file[f].precomp[0]->indextable = (char *)data;
            data += size[6 * f];
            if (split) {
                ptr->file[f].precomp[1]->indextable = (char *)data;
                data += size[6 * f + 3];
            }
        }

        for (f = 0; f < files; f++) {
            ptr->file[f].precomp[0]->sizetable = (ushort *)data;
            data += size[6 * f + 1];
            if (split) {
                ptr->file[f].precomp[1]->sizetable = (ushort *)data;
                data += size[6 * f + 4];
            }
        }

        for (f = 0; f < files; f++) {
            data = (ubyte *)((((uintptr_t)data) + 0x3f) & ~0x3f);
            ptr->file[f].precomp[0]->data = data;
            data += size[6 * f + 2];
            if (split) {
                data = (ubyte *)((((uintptr_t)data) + 0x3f) & ~0x3f);
                ptr->file[f].precomp[1]->data = data;
                data += size[6 * f + 5];
            }
        }
    }

    return 1;
}

static int init_table_dtz(struct TBEntry *entry)
{
    ubyte *data = (ubyte *)entry->data;
    ubyte *next;
    int f, s;
    uint64 tb_size[4];
    uint64 size[4 * 3];

    if (!data)
        return 0;

    if (data[0] != DTZ_MAGIC[0] ||
        data[1] != DTZ_MAGIC[1] ||
        data[2] != DTZ_MAGIC[2] ||
        data[3] != DTZ_MAGIC[3]) {
        lock_res << "Corrupted table.\n" << unlock_res;
        return 0;
    }

    int files = data[4] & 0x02 ? 4 : 1;

    data += 5;

    if (!entry->has_pawns) {
        struct DTZEntry_piece *ptr = (struct DTZEntry_piece *)entry;
        setup_pieces_piece_dtz(ptr, data, &tb_size[0]);
        data += ptr->num + 1;
        data += ((uintptr_t)data) & 0x01;

        ptr->precomp = setup_pairs(data, tb_size[0], &size[0], &next, &(ptr->flags), 0);
        data = next;

        ptr->map = data;
        if (ptr->flags & 2) {
            int i;
            for (i = 0; i < 4; i++) {
                ptr->map_idx[i] = static_cast<ushort>(data + 1 - ptr->map);
                data += 1 + data[0];
            }
            data += ((uintptr_t)data) & 0x01;
        }

        ptr->precomp->indextable = (char *)data;
        data += size[0];

        ptr->precomp->sizetable = (ushort *)data;
        data += size[1];

        data = (ubyte *)((((uintptr_t)data) + 0x3f) & ~0x3f);
        ptr->precomp->data = data;
        data += size[2];
    } else {
        struct DTZEntry_pawn *ptr = (struct DTZEntry_pawn *)entry;
        s = 1 + (ptr->pawns[1] > 0);
        for (f = 0; f < 4; f++) {
            setup_pieces_pawn_dtz(ptr, data, &tb_size[f], f);
            data += ptr->num + s;
        }
        data += ((uintptr_t)data) & 0x01;

        for (f = 0; f < files; f++) {
            ptr->file[f].precomp = setup_pairs(data, tb_size[f], &size[3 * f], &next, &(ptr->flags[f]), 0);
            data = next;
        }

        ptr->map = data;
        for (f = 0; f < files; f++) {
            if (ptr->flags[f] & 2) {
                int i;
                for (i = 0; i < 4; i++) {
                    ptr->map_idx[f][i] = static_cast<ushort>(data + 1 - ptr->map);
                    data += 1 + data[0];
                }
            }
        }
        data += ((uintptr_t)data) & 0x01;

        for (f = 0; f < files; f++) {
            ptr->file[f].precomp->indextable = (char *)data;
            data += size[3 * f];
        }

        for (f = 0; f < files; f++) {
            ptr->file[f].precomp->sizetable = (ushort *)data;
            data += size[3 * f + 1];
        }

        for (f = 0; f < files; f++) {
            data = (ubyte *)((((uintptr_t)data) + 0x3f) & ~0x3f);
            ptr->file[f].precomp->data = data;
            data += size[3 * f + 2];
        }
    }

    return 1;
}

template<bool LittleEndian>
static ubyte decompress_pairs(struct PairsData *d, uint64 idx)
{
    if (!d->idxbits)
        return ubyte(d->min_len);

    uint32 mainidx = static_cast<uint32>(idx >> d->idxbits);
    int litidx = (idx & ((1ULL << d->idxbits) - 1)) - (1ULL << (d->idxbits - 1));
    uint32 block = *(uint32 *)(d->indextable + 6 * mainidx);
    if (!LittleEndian)
        block = BSWAP32(block);

    ushort idxOffset = *(ushort *)(d->indextable + 6 * mainidx + 4);
    if (!LittleEndian)
        idxOffset = ushort((idxOffset << 8) | (idxOffset >> 8));
    litidx += idxOffset;

    if (litidx < 0) {
        do {
            litidx += d->sizetable[--block] + 1;
        } while (litidx < 0);
    } else {
        while (litidx > d->sizetable[block])
            litidx -= d->sizetable[block++] + 1;
    }

    uint32 *ptr = (uint32 *)(d->data + (block << d->blocksize));

    int m = d->min_len;
    ushort *offset = d->offset;
    base_t *base = d->base - m;
    ubyte *symlen = d->symlen;
    int sym, bitcnt;

    uint64 code = *((uint64 *)ptr);
    if (LittleEndian)
        code = BSWAP64(code);

    ptr += 2;
    bitcnt = 0; // number of "empty bits" in code
    for (;;) {
        int l = m;
        while (code < base[l]) l++;
        sym = offset[l];
        if (!LittleEndian)
            sym = ((sym & 0xff) << 8) | (sym >> 8);
        sym += static_cast<int>((code - base[l]) >> (64 - l));
        if (litidx < (int)symlen[sym] + 1) break;
        litidx -= (int)symlen[sym] + 1;
        code <<= l;
        bitcnt += l;
        if (bitcnt >= 32) {
            bitcnt -= 32;
            uint32 tmp = *ptr++;
            if (LittleEndian)
                tmp = BSWAP32(tmp);
            code |= ((uint64)tmp) << bitcnt;
        }
    }

    ubyte *sympat = d->sympat;
    while (symlen[sym] != 0) {
        ubyte* w = sympat + (3 * sym);
        int s1 = ((w[1] & 0xf) << 8) | w[0];
        if (litidx < (int)symlen[s1] + 1)
            sym = s1;
        else {
            litidx -= (int)symlen[s1] + 1;
            sym = (w[2] << 4) | (w[1] >> 4);
        }
    }

    return sympat[3 * sym];
}

void load_dtz_table(char *str, uint64 key1, uint64 key2)
{
    int i;
    struct TBEntry *ptr, *ptr3;
    struct TBHashEntry *ptr2;

    DTZ_table[0].key1 = key1;
    DTZ_table[0].key2 = key2;
    DTZ_table[0].entry = NULL;

    // find corresponding WDL entry
    ptr2 = TB_hash[key1 >> (64 - TBHASHBITS)];
    for (i = 0; i < HSHMAX; i++)
        if (ptr2[i].key == key1) break;
    if (i == HSHMAX) return;
    ptr = ptr2[i].ptr;

    ptr3 = (struct TBEntry *)malloc(ptr->has_pawns
                                    ? sizeof(struct DTZEntry_pawn)
                                    : sizeof(struct DTZEntry_piece));

    ptr3->data = map_file(str, DTZSUFFIX, &ptr3->mapping);
    ptr3->key = ptr->key;
    ptr3->num = ptr->num;
    ptr3->symmetric = ptr->symmetric;
    ptr3->has_pawns = ptr->has_pawns;
    if (ptr3->has_pawns) {
        struct DTZEntry_pawn *entry = (struct DTZEntry_pawn *)ptr3;
        entry->pawns[0] = ((struct TBEntry_pawn *)ptr)->pawns[0];
        entry->pawns[1] = ((struct TBEntry_pawn *)ptr)->pawns[1];
    } else {
        struct DTZEntry_piece *entry = (struct DTZEntry_piece *)ptr3;
        entry->enc_type = ((struct TBEntry_piece *)ptr)->enc_type;
    }
    if (!init_table_dtz(ptr3))
        free(ptr3);
    else
        DTZ_table[0].entry = ptr3;
}

static void free_wdl_entry(struct TBEntry *entry)
{
    unmap_file(entry->data, entry->mapping);
    if (!entry->has_pawns) {
        struct TBEntry_piece *ptr = (struct TBEntry_piece *)entry;
        free(ptr->precomp[0]);
        if (ptr->precomp[1])
            free(ptr->precomp[1]);
    } else {
        struct TBEntry_pawn *ptr = (struct TBEntry_pawn *)entry;
        int f;
        for (f = 0; f < 4; f++) {
            free(ptr->file[f].precomp[0]);
            if (ptr->file[f].precomp[1])
                free(ptr->file[f].precomp[1]);
        }
    }
}

static void free_dtz_entry(struct TBEntry *entry)
{
    unmap_file(entry->data, entry->mapping);
    if (!entry->has_pawns) {
        struct DTZEntry_piece *ptr = (struct DTZEntry_piece *)entry;
        free(ptr->precomp);
    } else {
        struct DTZEntry_pawn *ptr = (struct DTZEntry_pawn *)entry;
        int f;
        for (f = 0; f < 4; f++)
            free(ptr->file[f].precomp);
    }
    free(entry);
}

static int wdl_to_map[5] = { 1, 3, 0, 2, 0 };
static ubyte pa_flags[5] = { 8, 0, 0, 0, 4 };
//////////////#######################################/////////////////

namespace Zobrist {
  extern Key psq[PIECE_NB][SQUARE_NB];
}

int Tablebases::MaxCardinality = 0;

// Given a position with 6 or fewer pieces, produce a text string
// of the form KQPvKRP, where "KQP" represents the white pieces if
// mirror == 0 and the black pieces if mirror == 1.
static void prt_str(Position& pos, char *str, int mirror)
{
  Color color;
  PieceType pt;
  int i;

  color = !mirror ? WHITE : BLACK;
  for (pt = KING; pt >= PAWN; --pt)
    for (i = popcount(pos.pieces(color, pt)); i > 0; i--)
      *str++ = pchr[6 - pt];
  *str++ = 'v';
  color = ~color;
  for (pt = KING; pt >= PAWN; --pt)
    for (i = popcount(pos.pieces(color, pt)); i > 0; i--)
      *str++ = pchr[6 - pt];
  *str++ = 0;
}

// Given a position, produce a 64-bit material signature key.
// If the engine supports such a key, it should equal the engine's key.
static uint64 calc_key(Position& pos, int mirror)
{
  Color color;
  PieceType pt;
  int i;
  uint64 key = 0;

  color = !mirror ? WHITE : BLACK;
  for (pt = PAWN; pt <= KING; ++pt)
    for (i = popcount(pos.pieces(color, pt)); i > 0; i--)
      key ^= Zobrist::psq[make_piece(WHITE, pt)][i - 1];
  color = ~color;
  for (pt = PAWN; pt <= KING; ++pt)
    for (i = popcount(pos.pieces(color, pt)); i > 0; i--)
      key ^= Zobrist::psq[make_piece(BLACK, pt)][i - 1];

  return key;
}

// Produce a 64-bit material key corresponding to the material combination
// defined by pcs[16], where pcs[1], ..., pcs[6] is the number of white
// pawns, ..., kings and pcs[9], ..., pcs[14] is the number of black
// pawns, ..., kings.
uint64 calc_key_from_pcs(int *pcs, int mirror)
{
  int color;
  PieceType pt;
  int i;
  uint64 key = 0;

  color = !mirror ? 0 : 8;
  for (pt = PAWN; pt <= KING; ++pt)
    for (i = 0; i < pcs[color + pt]; i++)
      key ^= Zobrist::psq[make_piece(WHITE, pt)][i];
  color ^= 8;
  for (pt = PAWN; pt <= KING; ++pt)
    for (i = 0; i < pcs[color + pt]; i++)
      key ^= Zobrist::psq[make_piece(BLACK, pt)][i];

  return key;
}

bool is_little_endian() {
  union {
    int i;
    char c[sizeof(int)];
  } x;
  x.i = 1;
  return x.c[0] == 1;
}

static ubyte decompress_pairs(struct PairsData *d, uint64 idx)
{
  static const bool isLittleEndian = is_little_endian();
  return isLittleEndian ? decompress_pairs<true >(d, idx)
                        : decompress_pairs<false>(d, idx);
}

// probe_wdl_table and probe_dtz_table require similar adaptations.
static int probe_wdl_table(Position& pos, int *success)
{
  struct TBEntry *ptr;
  struct TBHashEntry *ptr2;
  uint64 idx;
  uint64 key;
  int i;
  ubyte res;
  int p[TBPIECES];

  // Obtain the position's material signature key.
  key = pos.material_key();

  // Test for KvK.
  if (key == (Zobrist::psq[W_KING][0] ^ Zobrist::psq[B_KING][0]))
    return 0;

  ptr2 = TB_hash[key >> (64 - TBHASHBITS)];
  for (i = 0; i < HSHMAX; i++)
    if (ptr2[i].key == key) break;
  if (i == HSHMAX) {
    *success = 0;
    return 0;
  }

  ptr = ptr2[i].ptr;
  if (!ptr->ready) {
    LOCK(TB_mutex);
    if (!ptr->ready) {
      char str[16];
      prt_str(pos, str, ptr->key != key);
      if (!init_table_wdl(ptr, str)) {
        ptr2[i].key = 0ULL;
        *success = 0;
        UNLOCK(TB_mutex);
        return 0;
      }
      // Memory barrier to ensure ptr->ready = 1 is not reordered.
#ifdef _MSC_VER
      _ReadWriteBarrier();
#else
      __asm__ __volatile__ ("" ::: "memory");
#endif
      ptr->ready = 1;
    }
    UNLOCK(TB_mutex);
  }

  int bside, mirror, cmirror;
  if (!ptr->symmetric) {
    if (key != ptr->key) {
      cmirror = 8;
      mirror = 0x38;
      bside = (pos.side_to_move() == WHITE);
    } else {
      cmirror = mirror = 0;
      bside = !(pos.side_to_move() == WHITE);
    }
  } else {
    cmirror = pos.side_to_move() == WHITE ? 0 : 8;
    mirror = pos.side_to_move() == WHITE ? 0 : 0x38;
    bside = 0;
  }

  // p[i] is to contain the square 0-63 (A1-H8) for a piece of type
  // pc[i] ^ cmirror, where 1 = white pawn, ..., 14 = black king.
  // Pieces of the same type are guaranteed to be consecutive.
  if (!ptr->has_pawns) {
    struct TBEntry_piece *entry = (struct TBEntry_piece *)ptr;
    ubyte *pc = entry->pieces[bside];
    for (i = 0; i < entry->num;) {
      Bitboard bb = pos.pieces((Color)((pc[i] ^ cmirror) >> 3),
                                      (PieceType)(pc[i] & 0x07));
      do {
        p[i++] = pop_lsb(&bb);
      } while (bb);
    }
    idx = encode_piece(entry, entry->norm[bside], p, entry->factor[bside]);
    res = decompress_pairs(entry->precomp[bside], idx);
  } else {
    struct TBEntry_pawn *entry = (struct TBEntry_pawn *)ptr;
    int k = entry->file[0].pieces[0][0] ^ cmirror;
    Bitboard bb = pos.pieces((Color)(k >> 3), (PieceType)(k & 0x07));
    i = 0;
    do {
      p[i++] = pop_lsb(&bb) ^ mirror;
    } while (bb);
    int f = pawn_file(entry, p);
    ubyte *pc = entry->file[f].pieces[bside];
    for (; i < entry->num;) {
      bb = pos.pieces((Color)((pc[i] ^ cmirror) >> 3),
                                    (PieceType)(pc[i] & 0x07));
      do {
        p[i++] = pop_lsb(&bb) ^ mirror;
      } while (bb);
    }
    idx = encode_pawn(entry, entry->file[f].norm[bside], p, entry->file[f].factor[bside]);
    res = decompress_pairs(entry->file[f].precomp[bside], idx);
  }

  return ((int)res) - 2;
}

static int probe_dtz_table(Position& pos, int wdl, int *success)
{
  struct TBEntry *ptr;
  uint64 idx;
  int i, res;
  int p[TBPIECES];

  // Obtain the position's material signature key.
  uint64 key = pos.material_key();

  if (DTZ_table[0].key1 != key && DTZ_table[0].key2 != key) {
    for (i = 1; i < DTZ_ENTRIES; i++)
      if (DTZ_table[i].key1 == key) break;
    if (i < DTZ_ENTRIES) {
      struct DTZTableEntry table_entry = DTZ_table[i];
      for (; i > 0; i--)
        DTZ_table[i] = DTZ_table[i - 1];
      DTZ_table[0] = table_entry;
    } else {
      struct TBHashEntry *ptr2 = TB_hash[key >> (64 - TBHASHBITS)];
      for (i = 0; i < HSHMAX; i++)
        if (ptr2[i].key == key) break;
      if (i == HSHMAX) {
        *success = 0;
        return 0;
      }
      ptr = ptr2[i].ptr;
      char str[16];
      int mirror = (ptr->key != key);
      prt_str(pos, str, mirror);
      if (DTZ_table[DTZ_ENTRIES - 1].entry)
        free_dtz_entry(DTZ_table[DTZ_ENTRIES-1].entry);
      for (i = DTZ_ENTRIES - 1; i > 0; i--)
        DTZ_table[i] = DTZ_table[i - 1];
      load_dtz_table(str, calc_key(pos, mirror), calc_key(pos, !mirror));
    }
  }

  ptr = DTZ_table[0].entry;
  if (!ptr) {
    *success = 0;
    return 0;
  }

  int bside, mirror, cmirror;
  if (!ptr->symmetric) {
    if (key != ptr->key) {
      cmirror = 8;
      mirror = 0x38;
      bside = (pos.side_to_move() == WHITE);
    } else {
      cmirror = mirror = 0;
      bside = !(pos.side_to_move() == WHITE);
    }
  } else {
    cmirror = pos.side_to_move() == WHITE ? 0 : 8;
    mirror = pos.side_to_move() == WHITE ? 0 : 0x38;
    bside = 0;
  }

  if (!ptr->has_pawns) {
    struct DTZEntry_piece *entry = (struct DTZEntry_piece *)ptr;
    if ((entry->flags & 1) != bside && !entry->symmetric) {
      *success = -1;
      return 0;
    }
    ubyte *pc = entry->pieces;
    for (i = 0; i < entry->num;) {
      Bitboard bb = pos.pieces((Color)((pc[i] ^ cmirror) >> 3),
                                    (PieceType)(pc[i] & 0x07));
      do {
        p[i++] = pop_lsb(&bb);
      } while (bb);
    }
    idx = encode_piece((struct TBEntry_piece *)entry, entry->norm, p, entry->factor);
    res = decompress_pairs(entry->precomp, idx);

    if (entry->flags & 2)
      res = entry->map[entry->map_idx[wdl_to_map[wdl + 2]] + res];

    if (!(entry->flags & pa_flags[wdl + 2]) || (wdl & 1))
      res *= 2;
  } else {
    struct DTZEntry_pawn *entry = (struct DTZEntry_pawn *)ptr;
    int k = entry->file[0].pieces[0] ^ cmirror;
    Bitboard bb = pos.pieces((Color)(k >> 3), (PieceType)(k & 0x07));
    i = 0;
    do {
      p[i++] = pop_lsb(&bb) ^ mirror;
    } while (bb);
    int f = pawn_file((struct TBEntry_pawn *)entry, p);
    if ((entry->flags[f] & 1) != bside) {
      *success = -1;
      return 0;
    }
    ubyte *pc = entry->file[f].pieces;
    for (; i < entry->num;) {
      bb = pos.pieces((Color)((pc[i] ^ cmirror) >> 3),
                            (PieceType)(pc[i] & 0x07));
      do {
        p[i++] = pop_lsb(&bb) ^ mirror;
      } while (bb);
    }
    idx = encode_pawn((struct TBEntry_pawn *)entry, entry->file[f].norm, p, entry->file[f].factor);
    res = decompress_pairs(entry->file[f].precomp, idx);

    if (entry->flags[f] & 2)
      res = entry->map[entry->map_idx[f][wdl_to_map[wdl + 2]] + res];

    if (!(entry->flags[f] & pa_flags[wdl + 2]) || (wdl & 1))
      res *= 2;
  }

  return res;
}

// Add underpromotion captures to list of captures.
static ExtMove *add_underprom_caps(Position& pos, ExtMove *stack, ExtMove *end)
{
  ExtMove *moves, *extra = end;

  for (moves = stack; moves < end; moves++) {
    Move move = moves->move;
    if (type_of(move) == PROMOTION && !pos.empty(to_sq(move))) {
      (*extra++).move = (Move)(move - (1 << 12));
      (*extra++).move = (Move)(move - (2 << 12));
      (*extra++).move = (Move)(move - (3 << 12));
    }
  }

  return extra;
}

static int probe_ab(Position& pos, int alpha, int beta, int *success)
{
  int v;
  ExtMove stack[64];
  ExtMove *moves, *end;
  StateInfo st;

  // Generate (at least) all legal non-ep captures including (under)promotions.
  // It is OK to generate more, as long as they are filtered out below.
  if (!pos.checkers()) {
    end = generate<CAPTURES>(pos, stack);
    // Since underpromotion captures are not included, we need to add them.
    end = add_underprom_caps(pos, stack, end);
  } else
    end = generate<EVASIONS>(pos, stack);

  for (moves = stack; moves < end; moves++) {
    Move capture = moves->move;
    if (!pos.capture(capture) || type_of(capture) == ENPASSANT
                        || !pos.legal(capture))
      continue;
    pos.do_move(capture, st, pos.gives_check(capture));
    v = -probe_ab(pos, -beta, -alpha, success);
    pos.undo_move(capture);
    if (*success == 0) return 0;
    if (v > alpha) {
      if (v >= beta) {
        *success = 2;
        return v;
      }
      alpha = v;
    }
  }

  v = probe_wdl_table(pos, success);
  if (*success == 0) return 0;
  if (alpha >= v) {
    *success = 1 + (alpha > 0);
    return alpha;
  } else {
    *success = 1;
    return v;
  }
}

// Probe the WDL table for a particular position.
// If *success != 0, the probe was successful.
// The return value is from the point of view of the side to move:
// -2 : loss
// -1 : loss, but draw under 50-move rule
//  0 : draw
//  1 : win, but draw under 50-move rule
//  2 : win
int Tablebases::probe_wdl(Position& pos, int *success)
{
  int v;

  *success = 1;
  v = probe_ab(pos, -2, 2, success);

  // If en passant is not possible, we are done.
  if (pos.ep_square() == SQ_NONE)
    return v;
  if (!(*success)) return 0;

  // Now handle en passant.
  int v1 = -3;
  // Generate (at least) all legal en passant captures.
  ExtMove stack[192];
  ExtMove *moves, *end;
  StateInfo st;

  if (!pos.checkers())
    end = generate<CAPTURES>(pos, stack);
  else
    end = generate<EVASIONS>(pos, stack);

  for (moves = stack; moves < end; moves++) {
    Move capture = moves->move;
    if (type_of(capture) != ENPASSANT
          || !pos.legal(capture))
      continue;
    pos.do_move(capture, st, pos.gives_check(capture));
    int v0 = -probe_ab(pos, -2, 2, success);
    pos.undo_move(capture);
    if (*success == 0) return 0;
    if (v0 > v1) v1 = v0;
  }
  if (v1 > -3) {
    if (v1 >= v) v = v1;
    else if (v == 0) {
      // Check whether there is at least one legal non-ep move.
      for (moves = stack; moves < end; moves++) {
        Move capture = moves->move;
        if (type_of(capture) == ENPASSANT) continue;
        if (pos.legal(capture)) break;
      }
      if (moves == end && !pos.checkers()) {
        end = generate<QUIETS>(pos, end);
        for (; moves < end; moves++) {
          Move move = moves->move;
          if (pos.legal(move))
            break;
        }
      }
      // If not, then we are forced to play the losing ep capture.
      if (moves == end)
        v = v1;
    }
  }

  return v;
}

// This routine treats a position with en passant captures as one without.
static int probe_dtz_no_ep(Position& pos, int *success)
{
  int wdl, dtz;

  wdl = probe_ab(pos, -2, 2, success);
  if (*success == 0) return 0;

  if (wdl == 0) return 0;

  if (*success == 2)
    return wdl == 2 ? 1 : 101;

  ExtMove stack[192];
  ExtMove *moves, *end = NULL;
  StateInfo st;

  if (wdl > 0) {
    // Generate at least all legal non-capturing pawn moves
    // including non-capturing promotions.
    if (!pos.checkers())
      end = generate<NON_EVASIONS>(pos, stack);
    else
      end = generate<EVASIONS>(pos, stack);

    for (moves = stack; moves < end; moves++) {
      Move move = moves->move;
      if (type_of(pos.moved_piece(move)) != PAWN || pos.capture(move)
                || !pos.legal(move))
        continue;
      pos.do_move(move, st, pos.gives_check(move));
      int v = -Tablebases::probe_wdl(pos, success);
      pos.undo_move(move);
      if (*success == 0) return 0;
      if (v == wdl)
        return v == 2 ? 1 : 101;
    }
  }

  dtz = 1 + probe_dtz_table(pos, wdl, success);
  if (*success >= 0) {
    if (wdl & 1) dtz += 100;
    return wdl >= 0 ? dtz : -dtz;
  }

  if (wdl > 0) {
    int best = 0xffff;
    for (moves = stack; moves < end; moves++) {
      Move move = moves->move;
      if (pos.capture(move) || type_of(pos.moved_piece(move)) == PAWN
                || !pos.legal(move))
        continue;
      pos.do_move(move, st, pos.gives_check(move));
      int v = -Tablebases::probe_dtz(pos, success);
      pos.undo_move(move);
      if (*success == 0) return 0;
      if (v > 0 && v + 1 < best)
        best = v + 1;
    }
    return best;
  } else {
    int best = -1;
    if (!pos.checkers())
      end = generate<NON_EVASIONS>(pos, stack);
    else
      end = generate<EVASIONS>(pos, stack);
    for (moves = stack; moves < end; moves++) {
      int v;
      Move move = moves->move;
      if (!pos.legal(move))
        continue;
      pos.do_move(move, st, pos.gives_check(move));
      if (st.rule50 == 0) {
        if (wdl == -2) v = -1;
        else {
          v = probe_ab(pos, 1, 2, success);
          v = (v == 2) ? 0 : -101;
        }
      } else {
        v = -Tablebases::probe_dtz(pos, success) - 1;
      }
      pos.undo_move(move);
      if (*success == 0) return 0;
      if (v < best)
        best = v;
    }
    return best;
  }
}

static int wdl_to_dtz[] = {
  -1, -101, 0, 101, 1
};

// Probe the DTZ table for a particular position.
// If *success != 0, the probe was successful.
// The return value is from the point of view of the side to move:
//         n < -100 : loss, but draw under 50-move rule
// -100 <= n < -1   : loss in n ply (assuming 50-move counter == 0)
//         0        : draw
//     1 < n <= 100 : win in n ply (assuming 50-move counter == 0)
//   100 < n        : win, but draw under 50-move rule
//
// The return value n can be off by 1: a return value -n can mean a loss
// in n+1 ply and a return value +n can mean a win in n+1 ply. This
// cannot happen for tables with positions exactly on the "edge" of
// the 50-move rule.
//
// This implies that if dtz > 0 is returned, the position is certainly
// a win if dtz + 50-move-counter <= 99. Care must be taken that the engine
// picks moves that preserve dtz + 50-move-counter <= 99.
//
// If n = 100 immediately after a capture or pawn move, then the position
// is also certainly a win, and during the whole phase until the next
// capture or pawn move, the inequality to be preserved is
// dtz + 50-movecounter <= 100.
//
// In short, if a move is available resulting in dtz + 50-move-counter <= 99,
// then do not accept moves leading to dtz + 50-move-counter == 100.
//
int Tablebases::probe_dtz(Position& pos, int *success)
{
  *success = 1;
  int v = probe_dtz_no_ep(pos, success);

  if (pos.ep_square() == SQ_NONE)
    return v;
  if (*success == 0) return 0;

  // Now handle en passant.
  int v1 = -3;

  ExtMove stack[192];
  ExtMove *moves, *end;
  StateInfo st;

  if (!pos.checkers())
    end = generate<CAPTURES>(pos, stack);
  else
    end = generate<EVASIONS>(pos, stack);

  for (moves = stack; moves < end; moves++) {
    Move capture = moves->move;
    if (type_of(capture) != ENPASSANT
                || !pos.legal(capture))
      continue;
    pos.do_move(capture, st, pos.gives_check(capture));
    int v0 = -probe_ab(pos, -2, 2, success);
    pos.undo_move(capture);
    if (*success == 0) return 0;
    if (v0 > v1) v1 = v0;
  }
  if (v1 > -3) {
    v1 = wdl_to_dtz[v1 + 2];
    if (v < -100) {
      if (v1 >= 0)
        v = v1;
    } else if (v < 0) {
      if (v1 >= 0 || v1 < -100)
        v = v1;
    } else if (v > 100) {
      if (v1 > 0)
        v = v1;
    } else if (v > 0) {
      if (v1 == 1)
        v = v1;
    } else if (v1 >= 0) {
      v = v1;
    } else {
      for (moves = stack; moves < end; moves++) {
        Move move = moves->move;
        if (type_of(move) == ENPASSANT) continue;
        if (pos.legal(move)) break;
      }
      if (moves == end && !pos.checkers()) {
        end = generate<QUIETS>(pos, end);
        for (; moves < end; moves++) {
          Move move = moves->move;
          if (pos.legal(move))
            break;
        }
      }
      if (moves == end)
        v = v1;
    }
  }

  return v;
}

// Check whether there has been at least one repetition of positions
// since the last capture or pawn move.
static int has_repeated(StateInfo *st)
{
  while (1) {
    int i = 4, e = std::min(st->rule50, st->pliesFromNull);
    if (e < i)
      return 0;
    StateInfo *stp = st->previous->previous;
    do {
      stp = stp->previous->previous;
      if (stp->key == st->key)
        return 1;
      i += 2;
    } while (i <= e);
    st = st->previous;
  }
}

static Value wdl_to_Value[5] = {
  -VALUE_MATE + MAX_PLY + 1,
  VALUE_DRAW - 2,
  VALUE_DRAW,
  VALUE_DRAW + 2,
  VALUE_MATE - MAX_PLY - 1
};

// Use the DTZ tables to filter out moves that don't preserve the win or draw.
// If the position is lost, but DTZ is fairly high, only keep moves that
// maximise DTZ.
//
// A return value false indicates that not all probes were successful and that
// no moves were filtered out.
bool Tablebases::root_probe(Position& pos, Search::RootMoves& rootMoves, Value& score)
{
  int success;

  int dtz = probe_dtz(pos, &success);
  if (!success) return false;

  StateInfo st;

  // Probe each move.
  for (size_t i = 0; i < rootMoves.size(); i++) {
    Move move = rootMoves[i].pv[0];
    pos.do_move(move, st, pos.gives_check(move));
    int v = 0;
    if (pos.checkers() && dtz > 0) {
      ExtMove s[192];
      if (generate<LEGAL>(pos, s) == s)
        v = 1;
    }
    if (!v) {
      if (st.rule50 != 0) {
        v = -Tablebases::probe_dtz(pos, &success);
        if (v > 0) v++;
        else if (v < 0) v--;
      } else {
        v = -Tablebases::probe_wdl(pos, &success);
        v = wdl_to_dtz[v + 2];
      }
    }
    pos.undo_move(move);
    if (!success) return false;
    rootMoves[i].score = (Value)v;
  }

  // Obtain 50-move counter for the root position.
  // In Stockfish there seems to be no clean way, so we do it like this:
  int cnt50 = st.previous->rule50;

  // Use 50-move counter to determine whether the root position is
  // won, lost or drawn.
  int wdl = 0;
  if (dtz > 0)
    wdl = (dtz + cnt50 <= 100) ? 2 : 1;
  else if (dtz < 0)
    wdl = (-dtz + cnt50 <= 100) ? -2 : -1;

  // Determine the score to report to the user.
  score = wdl_to_Value[wdl + 2];
  // If the position is winning or losing, but too few moves left, adjust the
  // score to show how close it is to winning or losing.
  // NOTE: int(PawnValueEg) is used as scaling factor in score_to_uci().
  if (wdl == 1 && dtz <= 100)
    score = (Value)(((200 - dtz - cnt50) * int(PawnValueEg)) / 200);
  else if (wdl == -1 && dtz >= -100)
    score = -(Value)(((200 + dtz - cnt50) * int(PawnValueEg)) / 200);

  // Now be a bit smart about filtering out moves.
  size_t j = 0;
  if (dtz > 0) { // winning (or 50-move rule draw)
    int best = 0xffff;
    for (size_t i = 0; i < rootMoves.size(); i++) {
      int v = rootMoves[i].score;
      if (v > 0 && v < best)
        best = v;
    }
    int max = best;
    // If the current phase has not seen repetitions, then try all moves
    // that stay safely within the 50-move budget, if there are any.
    if (!has_repeated(st.previous) && best + cnt50 <= 99)
      max = 99 - cnt50;
    for (size_t i = 0; i < rootMoves.size(); i++) {
      int v = rootMoves[i].score;
      if (v > 0 && v <= max)
        rootMoves[j++] = rootMoves[i];
    }
  } else if (dtz < 0) { // losing (or 50-move rule draw)
    int best = 0;
    for (size_t i = 0; i < rootMoves.size(); i++) {
      int v = rootMoves[i].score;
      if (v < best)
        best = v;
    }
    // Try all moves, unless we approach or have a 50-move rule draw.
    if (-best * 2 + cnt50 < 100)
      return true;
    for (size_t i = 0; i < rootMoves.size(); i++) {
      if (rootMoves[i].score == best)
        rootMoves[j++] = rootMoves[i];
    }
  } else { // drawing
    // Try all moves that preserve the draw.
    for (size_t i = 0; i < rootMoves.size(); i++) {
      if (rootMoves[i].score == 0)
        rootMoves[j++] = rootMoves[i];
    }
  }
  rootMoves.resize(j, Search::RootMove(MOVE_NONE));

  return true;
}

// Use the WDL tables to filter out moves that don't preserve the win or draw.
// This is a fallback for the case that some or all DTZ tables are missing.
//
// A return value false indicates that not all probes were successful and that
// no moves were filtered out.
bool Tablebases::root_probe_wdl(Position& pos, Search::RootMoves& rootMoves, Value& score)
{
  int success;

  int wdl = Tablebases::probe_wdl(pos, &success);
  if (!success) return false;
  score = wdl_to_Value[wdl + 2];

  StateInfo st;

  int best = -2;

  // Probe each move.
  for (size_t i = 0; i < rootMoves.size(); i++) {
    Move move = rootMoves[i].pv[0];
    pos.do_move(move, st, pos.gives_check(move));
    int v = -Tablebases::probe_wdl(pos, &success);
    pos.undo_move(move);
    if (!success) return false;
    rootMoves[i].score = (Value)v;
    if (v > best)
      best = v;
  }

  size_t j = 0;
  for (size_t i = 0; i < rootMoves.size(); i++) {
    if (rootMoves[i].score == best)
      rootMoves[j++] = rootMoves[i];
  }
  rootMoves.resize(j, Search::RootMove(MOVE_NONE));

  return true;
}

