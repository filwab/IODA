#ifndef __FEMU_FTL_H
#define __FEMU_FTL_H

#include "../nvme.h"
#include "./buffer.h"

#define INVALID_PPA     (~(0ULL))
#define INVALID_LPN     (~(0ULL))
#define UNMAPPED_PPA    (~(0ULL))

enum {
    NAND_READ =  0,
    NAND_WRITE = 1,
    NAND_ERASE = 2,

    NAND_READ_LATENCY = 40000,
    NAND_PROG_LATENCY = 200000,
    NAND_ERASE_LATENCY = 2000000,
};

enum {
    USER_IO = 0,
    GC_IO = 1,
};

enum {
    SEC_FREE = 0,
    SEC_INVALID = 1,
    SEC_VALID = 2,

    PG_FREE = 0,
    PG_INVALID = 1,
    PG_VALID = 2
};

enum {
    FEMU_ENABLE_GC_DELAY = 1,
    FEMU_DISABLE_GC_DELAY = 2,

    FEMU_ENABLE_DELAY_EMU = 3,
    FEMU_DISABLE_DELAY_EMU = 4,

    FEMU_RESET_ACCT = 5,
    FEMU_ENABLE_LOG = 6,
    FEMU_DISABLE_LOG = 7,

    FEMU_SYNC_GC = 8,
    FEMU_UNSYNC_GC = 9,

    FEMU_ENABLE_LOG_FREE_BLOCKS = 10,
    FEMU_DISABLE_LOG_FREE_BLOCKS = 11,

    FEMU_WINDOW_1S = 12,
    FEMU_WINDOW_100MS = 13,
    FEMU_WINDOW_2S = 14,
    FEMU_WINDOW_10MS = 15,
    FEMU_WINDOW_40MS = 16,
    FEMU_WINDOW_200MS = 17,
    FEMU_WINDOW_400MS = 18,

    FEMU_FAST_FAIL_SWITCH = 19,
    FEMU_STRAID_DEBUG_SWITCH = 20,

    NORMAL_MOD = 21,
    RECONS_MOD = 22,

	FEMU_PRINT_AND_RESET_COUNTERS = 23,

    FEMU_NAND_UTILIZATION_LOG = 24,

    FEMU_PRINT_CONFIG =25,

    BUFFER_MOD = 26,

    GROUP_GC_SWITCH = 27,

    SHOW_GROUP_INFO = 28,

    GC_STEERING_MOD = 29,

    GRPGC_RECON_MOD = 30,

    BACKUP_SPACE_ADD = 40,

    BACKUP_SPACE_DEL = 41
};


#define BLK_BITS    (16)
#define PG_BITS     (16)
#define SEC_BITS    (8)
#define PL_BITS     (8)
#define LUN_BITS    (8)
#define CH_BITS     (7)

/* describe a physical page addr */
struct ppa {
    union {
        struct {
            uint64_t blk : BLK_BITS;
            uint64_t pg  : PG_BITS;
            uint64_t sec : SEC_BITS;
            uint64_t pl  : PL_BITS;
            uint64_t lun : LUN_BITS;
            uint64_t ch  : CH_BITS;
            uint64_t rsv : 1;
        } g;

        uint64_t ppa;
    };
};

typedef int nand_sec_status_t;

struct nand_page {
    nand_sec_status_t *sec;
    int nsecs;
    int status;
};

struct nand_block {
    struct nand_page *pg;
    int npgs;
    int ipc; /* invalid page count */
    int vpc; /* valid page count */
    int erase_cnt;
    int wp; /* current write pointer */
};

struct nand_plane {
    struct nand_block *blk;
    int nblks;
};

struct nand_lun {
    struct nand_plane *pl;
    int npls;
    uint64_t next_lun_avail_time;
    bool busy;
    uint64_t gc_endtime;
};

struct ssd_channel {
    struct nand_lun *lun;
    int nluns;
    uint64_t next_ch_avail_time;
    bool busy;
    uint64_t gc_endtime;
};

struct ssdparams {
    int secsz;        /* sector size in bytes */
    int secs_per_pg;  /* # of sectors per page */
    int pgs_per_blk;  /* # of NAND pages per block */
    int blks_per_pl;  /* # of blocks per plane */
    int pls_per_lun;  /* # of planes per LUN (Die) */
    int luns_per_ch;  /* # of LUNs per channel */
    int nchs;         /* # of channels in the SSD */

    int pg_rd_lat;    /* NAND page read latency in nanoseconds */
    int pg_wr_lat;    /* NAND page program latency in nanoseconds */
    int blk_er_lat;   /* NAND block erase latency in nanoseconds */
    int ch_xfer_lat;  /* channel transfer latency for one page in nanoseconds
                       * this defines the channel bandwith
                       */

    double gc_thres_pcent;
    int gc_thres_lines;
    double gc_thres_pcent_high;
    int gc_thres_lines_high;
    bool enable_gc_delay;
    /*gql-enable gc window*/
    bool enable_gc_sync;
    int gc_sync_window;

    int fast_fail;
    int straid_debug;

    /* buffer read*/
    int buffer_read;
    int group_gc_sync;
    int gc_streering;//buffer_only mode ,no group_gc control.

    /* group-gc mode plus ioda-reconstruct mod */
    int grpgc_recon;
    


    /* below are all calculated values */
    int secs_per_blk; /* # of sectors per block */
    int secs_per_pl;  /* # of sectors per plane */
    int secs_per_lun; /* # of sectors per LUN */
    int secs_per_ch;  /* # of sectors per channel */
    int tt_secs;      /* # of sectors in the SSD */

    int pgs_per_pl;   /* # of pages per plane */
    int pgs_per_lun;  /* # of pages per LUN (Die) */
    int pgs_per_ch;   /* # of pages per channel */
    int tt_pgs;       /* total # of pages in the SSD */

    int blks_per_lun; /* # of blocks per LUN */
    int blks_per_ch;  /* # of blocks per channel */
    int tt_blks;      /* total # of blocks in the SSD */

    int secs_per_line;
    int pgs_per_line;
    int blks_per_line;
    int tt_lines;

    int pls_per_ch;   /* # of planes per channel */
    int tt_pls;       /* total # of planes in the SSD */

    int tt_luns;      /* total # of LUNs in the SSD */
};

typedef struct line {
    int id;  /* line id, the same as corresponding block id */
    int ipc; /* invalid page count in this line */
    int vpc; /* valid page count in this line */
    QTAILQ_ENTRY(line) entry; /* in either {free,victim,full} list */
    /* position in the priority queue for victim lines */
    size_t                  pos;
} line;

/* wp: record next write addr */
struct write_pointer {
    struct line *curline;
    int ch;
    int lun;
    int pg;
    int blk;
    int pl;
};

struct line_mgmt {
    struct line *lines;
    /* free line list, we only need to maintain a list of blk numbers */
    QTAILQ_HEAD(free_line_list, line) free_line_list;
    pqueue_t *victim_line_pq;
    //QTAILQ_HEAD(victim_line_list, line) victim_line_list;
    QTAILQ_HEAD(full_line_list, line) full_line_list;
    int tt_lines;
    int free_line_cnt;
    int victim_line_cnt;
    int full_line_cnt;
};

struct nand_cmd {
    int type;
    int cmd;
    int64_t stime; /* Coperd: request arrival time */
};

/* G-Dram_related */
struct dram {
    uint64_t dram_capacity;
    struct buffer_info* buffer;
    struct buffer_info* L_buffer;
};

#define SSD_NUM (8)
#define GROUP_SSD_NUM (2)


struct ssd {
    char *ssdname;
    struct ssdparams sp;
    struct ssd_channel *ch;
    struct ppa *maptbl; /* page level mapping table */
    uint64_t *rmap;     /* reverse mapptbl, assume it's stored in OOB */
    struct write_pointer wp;
    struct line_mgmt lm;
    
    /*dram_control*/
    struct dram dram;

    /*gql-params for gc-control*/
    uint16_t id; /* unique id for synchronization */
    int num_gc_in_s;
    int num_valid_pages_copied_s;
    uint64_t next_ssd_avail_time;
    uint64_t earliest_ssd_lun_avail_time;

    /*gql-读请求相关测试分析指标统计*/
    // For recording # FEMU level reads blocked by GC
    int total_reads;
    int num_reads_blocked_by_gc[SSD_NUM + 1];
    int total_gcs; //total gc num
    int reads_nor; //normal path reads num
    int reads_block; //blocked reads num
    int reads_recon; //reconstructed reads num
    int reads_reblk; //blocked reconstructed reads num


    /*gql- for utilizition use */
    uint32_t nand_utilization_log;
    uint64_t nand_end_time;
    uint64_t nand_read_pgs;
    uint64_t nand_write_pgs;
    uint64_t nand_erase_blks;
    uint64_t gc_read_pgs;
    uint64_t gc_write_pgs;
    uint64_t gc_erase_blks;

    /*gql- for group_gc_control */
    uint16_t bk_id; /* id of ssd for backup hot read data */
    uint16_t group_id; /* which group ssd in */

    /*gql- buffer statistic*/
    uint64_t total_read_num;
    uint64_t block_read_num;
    uint64_t total_read_pages;
    uint64_t buffer_page_num;
    uint64_t buffer_block_pages;

    /* lockless ring for communication with NVMe IO thread */
    struct rte_ring **to_ftl;
    struct rte_ring **to_poller;
    bool *dataplane_started_ptr;
    QemuThread ftl_thread;
};

// 1s
#define NAND_DIFF_TIME  (1000000000)

extern uint16_t ssd_id_cnt;

void ssd_init(FemuCtrl *n);
void ssd_reset_state(struct ssd *ssd);

/*******************BUFFER-FUNC*******************/

#define DRAM_CAPACITY (100*1024*1024) //100MB
#define DRAM_READ_LATENCY (1000)
#define DRAM_WRITE_LATENCY (1000)
#define LPN_IN_1GB (256*1024)
#define START_LPN_OP (16*256*1024)

#define MAX_L_NODE (2048)

uint64_t handle_buffer_read(struct ssd *ssd, struct ssd *target_ssd , uint64_t lpn, NvmeRequest *req);
uint64_t read_from_ssd(struct ssd *ssd, struct ssd *target_ssd , uint64_t lpn, NvmeRequest *req);
uint64_t write_to_buffer(struct ssd *ssd, struct ssd * target_ssd , uint64_t lpn, NvmeRequest *req);
uint64_t read_from_buffer(struct ssd *ssd, struct ssd * target_ssd , uint64_t lpn, NvmeRequest *req);

/****************BUFFER-FUNC-END*****************/

#ifdef FEMU_DEBUG_FTL
#define ftl_debug(fmt, ...) \
    do { printf("[FEMU] FTL-Dbg: " fmt, ## __VA_ARGS__); } while (0)
#else
#define ftl_debug(fmt, ...) \
    do { } while (0)
#endif

#define ftl_err(fmt, ...) \
    do { fprintf(stderr, "[FEMU] FTL-Err: " fmt, ## __VA_ARGS__); } while (0)

#define ftl_log(fmt, ...) \
    do { printf("[FEMU] FTL-Log: " fmt, ## __VA_ARGS__); } while (0)


/* FEMU assert() */
#ifdef FEMU_DEBUG_FTL
#define ftl_assert(expression) assert(expression)
#else
#define ftl_assert(expression)
#endif

#endif
