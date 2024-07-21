#include "ftl.h"
#include <stdint.h>

//#define FEMU_DEBUG_FTL

uint16_t ssd_id_cnt = 0;//g-盘数量
struct ssd *ssd_array[SSD_NUM];//g-包含了 SSD_NUM 个ssd的结构体数组
uint64_t gc_endtime_array[SSD_NUM];//g-原ioda中用于统计阵列中盘同时gc的频率和情况

static void *ftl_thread(void *arg);
static uint64_t Idle_buffer_read(struct ssd *ssd, struct ssd *target_ssd, uint64_t lpn, NvmeRequest *req);
static uint64_t busy_buffer_read(struct ssd *ssd, struct ssd *target_ssd, uint64_t lpn, NvmeRequest *req);
static int delete_from_buffer(struct ssd *ssd, uint64_t lpn);

static inline uint64_t get_low32(uint64_t data)
{
    return (data & 0xFFFFFFFF);

}
static inline uint64_t get_high32(uint64_t data)
{
    return (data >> 32);

}
static inline uint64_t set_low32(uint64_t data, uint64_t low32)
{
    return ((data & 0xFFFFFFFF00000000) | (low32 & 0xFFFFFFFF));
}

static inline bool should_gc(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct ssd *ssd)
{
    return (ssd->lm.free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn)
{
    return ssd->maptbl[lpn];
}

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    ftl_assert(lpn < ssd->sp.tt_pgs);
    ssd->maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t pgidx;

    pgidx = ppa->g.ch  * spp->pgs_per_ch  + \
            ppa->g.lun * spp->pgs_per_lun + \
            ppa->g.pl  * spp->pgs_per_pl  + \
            ppa->g.blk * spp->pgs_per_blk + \
            ppa->g.pg;

    ftl_assert(pgidx < spp->tt_pgs);

    return pgidx;
}

static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    return ssd->rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa)
{
    uint64_t pgidx = ppa2pgidx(ssd, ppa);

    ssd->rmap[pgidx] = lpn;
}

static inline int victim_line_cmp_pri(pqueue_pri_t next, pqueue_pri_t curr)
{
    return (next > curr);
}

static inline pqueue_pri_t victim_line_get_pri(void *a)
{
    return ((struct line *)a)->vpc;
}

static inline void victim_line_set_pri(void *a, pqueue_pri_t pri)
{
    ((struct line *)a)->vpc = pri;
}

static inline size_t victim_line_get_pos(void *a)
{
    return ((struct line *)a)->pos;
}

static inline void victim_line_set_pos(void *a, size_t pos)
{
    ((struct line *)a)->pos = pos;
}

static inline struct buffer_group* buffer_search(tAVLTree *buffer, uint64_t lpn) {
    struct buffer_group node;
    node.key = lpn;
    return (struct buffer_group*)avlTreeFind(buffer, (TREE_NODE *)&node);
}

static void ssd_init_lines(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *line;

    lm->tt_lines = spp->blks_per_pl;
    ftl_assert(lm->tt_lines == spp->tt_lines);
    lm->lines = g_malloc0(sizeof(struct line) * lm->tt_lines);

    QTAILQ_INIT(&lm->free_line_list);
    lm->victim_line_pq = pqueue_init(spp->tt_lines, victim_line_cmp_pri,
            victim_line_get_pri, victim_line_set_pri,
            victim_line_get_pos, victim_line_set_pos);
    QTAILQ_INIT(&lm->full_line_list);

    lm->free_line_cnt = 0;
    for (int i = 0; i < lm->tt_lines; i++) {
        line = &lm->lines[i];
        line->id = i;
        line->ipc = 0;
        line->vpc = 0;
        line->pos = 0;
        /* initialize all the lines as free lines */
        QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
        lm->free_line_cnt++;
    }

    ftl_assert(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;
}

static void ssd_init_write_pointer(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;

    /* wpp->curline is always our next-to-write super-block */
    wpp->curline = curline;
    wpp->ch = 0;
    wpp->lun = 0;
    wpp->pg = 0;
    wpp->blk = 0;
    wpp->pl = 0;
}

static inline void check_addr(int a, int max)
{
    ftl_assert(a >= 0 && a < max);
}

static struct line *get_next_free_line(struct ssd *ssd)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *curline = NULL;

    curline = QTAILQ_FIRST(&lm->free_line_list);
    if (!curline) {
        ftl_err("No free lines left in [%s] !!!!\n", ssd->ssdname);
        return NULL;
    }

    QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
    lm->free_line_cnt--;
    return curline;
}

static void ssd_advance_write_pointer(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;
    struct write_pointer *wpp = &ssd->wp;
    struct line_mgmt *lm = &ssd->lm;

    check_addr(wpp->ch, spp->nchs);
    wpp->ch++;
    if (wpp->ch == spp->nchs) {
        wpp->ch = 0;
        check_addr(wpp->lun, spp->luns_per_ch);
        wpp->lun++;
        /* in this case, we should go to next lun */
        if (wpp->lun == spp->luns_per_ch) {
            wpp->lun = 0;
            /* go to next page in the block */
            check_addr(wpp->pg, spp->pgs_per_blk);
            wpp->pg++;
            if (wpp->pg == spp->pgs_per_blk) {
                wpp->pg = 0;
                /* move current line to {victim,full} line list */
                if (wpp->curline->vpc == spp->pgs_per_line) {
                    /* all pgs are still valid, move to full line list */
                    ftl_assert(wpp->curline->ipc == 0);
                    QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
                    lm->full_line_cnt++;
                } else {
                    ftl_assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line);
                    /* there must be some invalid pages in this line */
                    ftl_assert(wpp->curline->ipc > 0);
                    pqueue_insert(lm->victim_line_pq, wpp->curline);
                    lm->victim_line_cnt++;
                }
                /* current line is used up, pick another empty line */
                check_addr(wpp->blk, spp->blks_per_pl);
                wpp->curline = NULL;
                wpp->curline = get_next_free_line(ssd);
                if (!wpp->curline) {
                    /* TODO */
                    abort();
                }
                wpp->blk = wpp->curline->id;
                check_addr(wpp->blk, spp->blks_per_pl);
                /* make sure we are starting from page 0 in the super block */
                ftl_assert(wpp->pg == 0);
                ftl_assert(wpp->lun == 0);
                ftl_assert(wpp->ch == 0);
                /* TODO: assume # of pl_per_lun is 1, fix later */
                ftl_assert(wpp->pl == 0);
            }
        }
    }
}

static struct ppa get_new_page(struct ssd *ssd)
{
    struct write_pointer *wpp = &ssd->wp;
    struct ppa ppa;
    ppa.ppa = 0;
    ppa.g.ch = wpp->ch;
    ppa.g.lun = wpp->lun;
    ppa.g.pg = wpp->pg;
    ppa.g.blk = wpp->blk;
    ppa.g.pl = wpp->pl;
    ftl_assert(ppa.g.pl == 0);

    return ppa;
}

static void check_params(struct ssdparams *spp)
{
    /*
     * we are using a general write pointer increment method now, no need to
     * force luns_per_ch and nchs to be power of 2
     */

    //ftl_assert(is_power_of_2(spp->luns_per_ch));
    //ftl_assert(is_power_of_2(spp->nchs));
}

static void ssd_init_params(struct ssdparams *spp)
{
    spp->secsz = 512;
    spp->secs_per_pg = 8;
    spp->pgs_per_blk = 256;
    spp->blks_per_pl = 320; /* gql-16GB-to-20GB */
    spp->pls_per_lun = 1;
    spp->luns_per_ch = 8;
    spp->nchs = 8;

    spp->pg_rd_lat = NAND_READ_LATENCY;
    spp->pg_wr_lat = NAND_PROG_LATENCY;
    spp->blk_er_lat = NAND_ERASE_LATENCY;
    spp->ch_xfer_lat = 0;

    /* calculated values */
    spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
    spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
    spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
    spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
    spp->tt_secs = spp->secs_per_ch * spp->nchs;

    spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
    spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
    spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
    spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

    spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
    spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
    spp->tt_blks = spp->blks_per_ch * spp->nchs;

    spp->pls_per_ch =  spp->pls_per_lun * spp->luns_per_ch;
    spp->tt_pls = spp->pls_per_ch * spp->nchs;

    spp->tt_luns = spp->luns_per_ch * spp->nchs;

    /* line is special, put it at the end */
    spp->blks_per_line = spp->tt_luns; /* TODO: to fix under multiplanes */
    spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
    spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
    spp->tt_lines = spp->blks_per_lun; /* TODO: to fix under multiplanes */

    /*gql- gc threshold and windows control */
    spp->gc_thres_pcent = 0.75;
    spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
    spp->gc_thres_pcent_high = 0.95;
    spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
    spp->enable_gc_delay = true;
    spp->enable_gc_sync = false;
    spp->gc_sync_window = 100;
    spp->fast_fail = 0;
    spp->straid_debug = 0;
    spp->buffer_read = 0;
    spp->group_gc_sync = 0;
    spp->gc_streering = 0;

    spp->grpgc_recon = 0;

    check_params(spp);
}

static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp)
{
    pg->nsecs = spp->secs_per_pg;
    pg->sec = g_malloc0(sizeof(nand_sec_status_t) * pg->nsecs);
    for (int i = 0; i < pg->nsecs; i++) {
        pg->sec[i] = SEC_FREE;
    }
    pg->status = PG_FREE;
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp)
{
    blk->npgs = spp->pgs_per_blk;
    blk->pg = g_malloc0(sizeof(struct nand_page) * blk->npgs);
    for (int i = 0; i < blk->npgs; i++) {
        ssd_init_nand_page(&blk->pg[i], spp);
    }
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt = 0;
    blk->wp = 0;
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp)
{
    pl->nblks = spp->blks_per_pl;
    pl->blk = g_malloc0(sizeof(struct nand_block) * pl->nblks);
    for (int i = 0; i < pl->nblks; i++) {
        ssd_init_nand_blk(&pl->blk[i], spp);
    }
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp)
{
    lun->npls = spp->pls_per_lun;
    lun->pl = g_malloc0(sizeof(struct nand_plane) * lun->npls);
    for (int i = 0; i < lun->npls; i++) {
        ssd_init_nand_plane(&lun->pl[i], spp);
    }
    lun->next_lun_avail_time = 0;
    lun->busy = false;
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp)
{
    ch->nluns = spp->luns_per_ch;
    ch->lun = g_malloc0(sizeof(struct nand_lun) * ch->nluns);
    for (int i = 0; i < ch->nluns; i++) {
        ssd_init_nand_lun(&ch->lun[i], spp);
    }
    ch->next_ch_avail_time = 0;
    ch->busy = 0;
}

static void ssd_init_maptbl(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->maptbl = g_malloc0(sizeof(struct ppa) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }
}

static void ssd_init_rmap(struct ssd *ssd)
{
    struct ssdparams *spp = &ssd->sp;

    ssd->rmap = g_malloc0(sizeof(uint64_t) * spp->tt_pgs);
    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }
}

static void init_dram(struct ssd *ssd)
{
    ssd->dram.dram_capacity= DRAM_CAPACITY;
    /* initilize buffer table*/
    ssd->dram.buffer = (tAVLTree*)avlTreeCreate((void*)keyCompareFunc, (void*)freeFunc);
    ssd->dram.buffer->buffer_full_flag = 0;
    ssd->dram.buffer->max_buffer_page = (unsigned int)(ssd->dram.dram_capacity / 4096);
    ssd->dram.buffer->buffer_page_count = 0;

    /* initilize L_buffer for LRU-2 algorithm*/
    ssd->dram.L_buffer = (tAVLTree*)avlTreeCreate((void*)keyCompareFunc, (void*)freeFunc);
    ssd->dram.L_buffer->buffer_full_flag = 0;
    ssd->dram.L_buffer->max_buffer_page = MAX_L_NODE;
    ssd->dram.L_buffer->buffer_page_count = 0;

}

void ssd_init(FemuCtrl *n)
{
    struct ssd *ssd = n->ssd;
    struct ssdparams *spp = &ssd->sp;

    ftl_assert(ssd);

    ssd_init_params(spp);

    /*gql- assign ssd id for GC synchronization */
    ssd->id = ssd_id_cnt;
    ssd->bk_id = (ssd_id_cnt + GROUP_SSD_NUM) % SSD_NUM;
    ssd->group_id = (ssd_id_cnt / GROUP_SSD_NUM) ;
	ssd_array[ssd->id] = ssd;
    ssd_id_cnt++;
    ftl_log("GCSYNC SSD initialized with id %d\n", ssd->id);
	ssd->next_ssd_avail_time = 0;
	ssd->earliest_ssd_lun_avail_time = UINT64_MAX;
	gc_endtime_array[ssd->id] = 0;

    /*gql-utilizition params initial*/
    ssd->nand_utilization_log = 0;
    ssd->nand_end_time = 0;
    ssd->nand_read_pgs = 0;
    ssd->nand_write_pgs = 0;
    ssd->nand_erase_blks = 0;
    ssd->gc_read_pgs = 0;
    ssd->gc_write_pgs = 0;
    ssd->gc_erase_blks = 0;

    /*gql- read_perfrm  prams  initial*/
    for (int i = 0; i <= SSD_NUM; i++) {
	    ssd->num_reads_blocked_by_gc[i] = 0;
    }

    ssd->total_reads = 0;
    ssd->total_gcs = 0;
    ssd->reads_nor = 0; //正常读请求数量
    ssd->reads_block = 0; //阻塞读请求数量
    ssd->reads_recon = 0; //重构读请求数量
    ssd->reads_reblk = 0; //重构被阻塞读请求数量

    /* initialize dram */
    init_dram(ssd);

    ssd->total_read_num = 0;
    ssd->block_read_num = 0;
    ssd->total_read_pages = 0;
    ssd->buffer_page_num = 0;
    ssd->buffer_block_pages = 0;

    /* initialize ssd internal layout architecture */
    ssd->ch = g_malloc0(sizeof(struct ssd_channel) * spp->nchs);
    for (int i = 0; i < spp->nchs; i++) {
        ssd_init_ch(&ssd->ch[i], spp);
    }

    /* initialize maptbl */
    ssd_init_maptbl(ssd);

    /* initialize rmap */
    ssd_init_rmap(ssd);

    /* initialize all the lines */
    ssd_init_lines(ssd);

    /* initialize write pointer, this is how we allocate new pages for writes */
    ssd_init_write_pointer(ssd);

    qemu_thread_create(&ssd->ftl_thread, "FEMU-FTL-Thread", ftl_thread, n,
                       QEMU_THREAD_JOINABLE);
}
void ssd_reset_state(struct ssd *ssd) {
    struct ssdparams *spp = &ssd->sp;

    ssd->next_ssd_avail_time = 0;
    ssd->earliest_ssd_lun_avail_time = UINT64_MAX;
    gc_endtime_array[ssd->id] = 0;

    ssd->nand_utilization_log = 0;
    ssd->nand_end_time = 0;
    ssd->nand_read_pgs = 0;
    ssd->nand_write_pgs = 0;
    ssd->nand_erase_blks = 0;
    ssd->gc_read_pgs = 0;
    ssd->gc_write_pgs = 0;
    ssd->gc_erase_blks = 0;

    for (int i = 0; i <= SSD_NUM; i++) {
        ssd->num_reads_blocked_by_gc[i] = 0;
    }

    ssd->total_reads = 0;
    ssd->total_gcs = 0;
    ssd->reads_nor = 0;
    ssd->reads_block = 0;
    ssd->reads_recon = 0;
    ssd->reads_reblk = 0;

    for (int i = 0; i < spp->nchs; i++) {
        struct ssd_channel *ch = &ssd->ch[i];
        ch->next_ch_avail_time = 0;
        ch->busy = 0;

        for (int j = 0; j < spp->luns_per_ch; j++) {
            struct nand_lun *lun = &ch->lun[j];
            lun->next_lun_avail_time = 0;
            lun->busy = false;

            for (int k = 0; k < spp->pls_per_lun; k++) {
                struct nand_plane *pl = &lun->pl[k];

                for (int l = 0; l < spp->blks_per_pl; l++) {
                    struct nand_block *blk = &pl->blk[l];
                    blk->ipc = 0;
                    blk->vpc = 0;
                    blk->erase_cnt = 0;
                    blk->wp = 0;

                    for (int m = 0; m < spp->pgs_per_blk; m++) {
                        struct nand_page *pg = &blk->pg[m];
                        pg->status = PG_FREE;
                        for (int n = 0; n < pg->nsecs; n++) {
                            pg->sec[n] = SEC_FREE;
                        }
                    }
                }
            }
        }
    }

    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->maptbl[i].ppa = UNMAPPED_PPA;
    }

    for (int i = 0; i < spp->tt_pgs; i++) {
        ssd->rmap[i] = INVALID_LPN;
    }

    struct line_mgmt *lm = &ssd->lm;
    QTAILQ_INIT(&lm->free_line_list);
    QTAILQ_INIT(&lm->full_line_list);
    lm->free_line_cnt = 0;
    for (int i = 0; i < lm->tt_lines; i++) {
        struct line *line = &lm->lines[i];
        line->ipc = 0;
        line->vpc = 0;
        line->pos = 0;
        QTAILQ_INSERT_TAIL(&ssd->lm.free_line_list, line, entry);
        lm->free_line_cnt++;
    }
    ftl_assert(lm->free_line_cnt == lm->tt_lines);
    lm->victim_line_cnt = 0;
    lm->full_line_cnt = 0;

    ssd_init_write_pointer(ssd);

}

static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    int ch = ppa->g.ch;
    int lun = ppa->g.lun;
    int pl = ppa->g.pl;
    int blk = ppa->g.blk;
    int pg = ppa->g.pg;
    int sec = ppa->g.sec;

    if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch && pl >=
        0 && pl < spp->pls_per_lun && blk >= 0 && blk < spp->blks_per_pl && pg
        >= 0 && pg < spp->pgs_per_blk && sec >= 0 && sec < spp->secs_per_pg)
        return true;

    return false;
}

static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn)
{
    return (lpn < ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa)
{
    return !(ppa->ppa == UNMAPPED_PPA);
}

static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa)
{
    struct ssd_channel *ch = get_ch(ssd, ppa);
    return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_lun *lun = get_lun(ssd, ppa);
    return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_plane *pl = get_pl(ssd, ppa);
    return &(pl->blk[ppa->g.blk]);
}

static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa)
{
    return &(ssd->lm.lines[ppa->g.blk]);
}

static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = get_blk(ssd, ppa);
    return &(blk->pg[ppa->g.pg]);
}

static inline uint64_t lpn_trans(uint64_t lpn)
{
    /* Mapping user_lpn on one disc to 1GB of op space on another disc */
    return (lpn % LPN_IN_1GB) + START_LPN_OP;
}

static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct
        nand_cmd *ncmd)
{
    int c = ncmd->cmd;
    uint64_t cmd_stime = (ncmd->stime == 0) ? \
        qemu_clock_get_ns(QEMU_CLOCK_REALTIME) : ncmd->stime;
    uint64_t nand_stime;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lun = get_lun(ssd, ppa);
    uint64_t lat = 0;

    switch (c) {
    case NAND_READ:
        /* read: perform NAND cmd first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;
        lat = lun->next_lun_avail_time - cmd_stime;
#if 0
        lun->next_lun_avail_time = nand_stime + spp->pg_rd_lat;

        /* read: then data transfer through channel */
        chnl_stime = (ch->next_ch_avail_time < lun->next_lun_avail_time) ? \
            lun->next_lun_avail_time : ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        lat = ch->next_ch_avail_time - cmd_stime;
#endif
        break;

    case NAND_WRITE:
        /* write: transfer data through channel first */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        if (ncmd->type == USER_IO) {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        } else {
            lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;
        }
        lat = lun->next_lun_avail_time - cmd_stime;

#if 0
        chnl_stime = (ch->next_ch_avail_time < cmd_stime) ? cmd_stime : \
                     ch->next_ch_avail_time;
        ch->next_ch_avail_time = chnl_stime + spp->ch_xfer_lat;

        /* write: then do NAND program */
        nand_stime = (lun->next_lun_avail_time < ch->next_ch_avail_time) ? \
            ch->next_ch_avail_time : lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->pg_wr_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
#endif
        break;

    case NAND_ERASE:
        /* erase: only need to advance NAND status */
        nand_stime = (lun->next_lun_avail_time < cmd_stime) ? cmd_stime : \
                     lun->next_lun_avail_time;
        lun->next_lun_avail_time = nand_stime + spp->blk_er_lat;

        lat = lun->next_lun_avail_time - cmd_stime;
        break;

    default:
        ftl_err("Unsupported NAND command: 0x%x\n", c);
    }
    
    if (lun->next_lun_avail_time > ssd->next_ssd_avail_time) {
		ssd->next_ssd_avail_time = lun->next_lun_avail_time;
	}
	if (lun->next_lun_avail_time < ssd->earliest_ssd_lun_avail_time) {
		ssd->earliest_ssd_lun_avail_time = lun->next_lun_avail_time;
	}

    if (ssd->nand_utilization_log) {
        if (nand_stime > ssd->nand_end_time) {
            ftl_log("%s ~%lus, r%.1f w%.1f e%.1f %lu%%, [r%.1f w%.1f e%.1f %lu%%](MB/s)\n",
                    ssd->ssdname, ssd->nand_end_time / 1000000000,
                    ssd->nand_read_pgs*(4)*(1000000000.0)/(NAND_DIFF_TIME*(1024.0)), ssd->nand_write_pgs*(4)*(1000000000.0)/(NAND_DIFF_TIME*(1024.0)), 
                    ssd->nand_erase_blks*(1024)*(1000000000.0)/(NAND_DIFF_TIME*(1024.0)),
                    100 *
                        (ssd->nand_read_pgs * (uint64_t)spp->pg_rd_lat +
                            ssd->nand_write_pgs * (uint64_t)spp->pg_wr_lat +
                            ssd->nand_erase_blks * (uint64_t)spp->blk_er_lat) /
                        ((uint64_t)NAND_DIFF_TIME * (uint64_t)spp->tt_luns),
                    ssd->gc_read_pgs*(4)*(1000000000.0)/(NAND_DIFF_TIME*(1024.0)), ssd->gc_write_pgs*(4)*(1000000000.0)/(NAND_DIFF_TIME*(1024.0)),
                    ssd->gc_erase_blks*(1024)*(1000000000.0)/(NAND_DIFF_TIME*(1024.0)),
                    100 *
                        (ssd->gc_read_pgs * (uint64_t)spp->pg_rd_lat +
                            ssd->gc_write_pgs * (uint64_t)spp->pg_wr_lat +
                            ssd->gc_erase_blks * (uint64_t)spp->blk_er_lat) /
                        ((uint64_t)NAND_DIFF_TIME * (uint64_t)spp->tt_luns));
            ssd->nand_end_time =
                nand_stime - nand_stime % NAND_DIFF_TIME + NAND_DIFF_TIME;
            ssd->gc_read_pgs = 0;
            ssd->gc_write_pgs = 0;
            ssd->gc_erase_blks = 0;
            ssd->nand_read_pgs = 0;
            ssd->nand_write_pgs = 0;
            ssd->nand_erase_blks = 0;
        }
        if (ncmd->type == GC_IO) {
            ssd->gc_read_pgs += (c == NAND_READ);
            ssd->gc_write_pgs += (c == NAND_WRITE);
            ssd->gc_erase_blks += (c == NAND_ERASE);
        }
        ssd->nand_read_pgs += (c == NAND_READ);
        ssd->nand_write_pgs += (c == NAND_WRITE);
        ssd->nand_erase_blks += (c == NAND_ERASE);
    }


    return lat;
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    bool was_full_line = false;
    struct line *line;

    /* update corresponding page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_VALID);
    pg->status = PG_INVALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
    blk->ipc++;
    ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
    blk->vpc--;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line);
    if (line->vpc == spp->pgs_per_line) {
        ftl_assert(line->ipc == 0);
        was_full_line = true;
    }
    line->ipc++;
    ftl_assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line);
    /* Adjust the position of the victime line in the pq under over-writes */
    if (line->pos) {
        /* Note that line->vpc will be updated by this call */
        pqueue_change_priority(lm->victim_line_pq, line->vpc - 1, line);
    } else {
        line->vpc--;
    }

    if (was_full_line) {
        /* move line: "full" -> "victim" */
        QTAILQ_REMOVE(&lm->full_line_list, line, entry);
        lm->full_line_cnt--;
        pqueue_insert(lm->victim_line_pq, line);
        lm->victim_line_cnt++;
    }
}

static void mark_page_valid(struct ssd *ssd, struct ppa *ppa)
{
    struct nand_block *blk = NULL;
    struct nand_page *pg = NULL;
    struct line *line;

    /* update page status */
    pg = get_pg(ssd, ppa);
    ftl_assert(pg->status == PG_FREE);
    pg->status = PG_VALID;

    /* update corresponding block status */
    blk = get_blk(ssd, ppa);
    ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
    blk->vpc++;

    /* update corresponding line status */
    line = get_line(ssd, ppa);
    ftl_assert(line->vpc >= 0 && line->vpc < ssd->sp.pgs_per_line);
    line->vpc++;
}

static void mark_block_free(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_block *blk = get_blk(ssd, ppa);
    struct nand_page *pg = NULL;

    for (int i = 0; i < spp->pgs_per_blk; i++) {
        /* reset page status */
        pg = &blk->pg[i];
        ftl_assert(pg->nsecs == spp->secs_per_pg);
        pg->status = PG_FREE;
    }

    /* reset block status */
    ftl_assert(blk->npgs == spp->pgs_per_blk);
    blk->ipc = 0;
    blk->vpc = 0;
    blk->erase_cnt++;
}

static void gc_read_page(struct ssd *ssd, struct ppa *ppa)
{
    /* advance ssd status, we don't care about how long it takes */
    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcr;
        gcr.type = GC_IO;
        gcr.cmd = NAND_READ;
        gcr.stime = 0;
        ssd_advance_status(ssd, ppa, &gcr);
    }
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa)
{
    struct ppa new_ppa;
    struct nand_lun *new_lun;
    uint64_t lpn = get_rmap_ent(ssd, old_ppa);

    ftl_assert(valid_lpn(ssd, lpn));
    new_ppa = get_new_page(ssd);
    /* update maptbl */
    set_maptbl_ent(ssd, lpn, &new_ppa);
    /* update rmap */
    set_rmap_ent(ssd, lpn, &new_ppa);

    mark_page_valid(ssd, &new_ppa);

    /* need to advance the write pointer here */
    ssd_advance_write_pointer(ssd);

    if (ssd->sp.enable_gc_delay) {
        struct nand_cmd gcw;
        gcw.type = GC_IO;
        gcw.cmd = NAND_WRITE;
        gcw.stime = 0;
        ssd_advance_status(ssd, &new_ppa, &gcw);
    }

    /* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

    new_lun = get_lun(ssd, &new_ppa);
    new_lun->gc_endtime = new_lun->next_lun_avail_time;

    return 0;
}

static struct line *select_victim_line(struct ssd *ssd, bool force)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *victim_line = NULL;

    victim_line = pqueue_peek(lm->victim_line_pq);
    if (!victim_line) {
        return NULL;
    }

    /*gql- change 8 to 4 当line中无效页面的数量小于行中总页面数量的25%（即1/4）时，才会考虑对这个行进行垃圾回收*/
    if (!force && victim_line->ipc < ssd->sp.pgs_per_line / 8) {
        return NULL;
    }

    pqueue_pop(lm->victim_line_pq);
    victim_line->pos = 0;
    lm->victim_line_cnt--;

    /* victim_line is a danggling node now */
    return victim_line;
}

/* here ppa identifies the block we want to clean */
/*    Returns the number of valid pages we needed to copy within the block*/
static int clean_one_block(struct ssd *ssd, struct ppa *ppa)
{
    struct ssdparams *spp = &ssd->sp;
    struct nand_page *pg_iter = NULL;
    int cnt = 0;

    for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
        ppa->g.pg = pg;
        pg_iter = get_pg(ssd, ppa);
        /* there shouldn't be any free page in victim blocks */
        ftl_assert(pg_iter->status != PG_FREE);
        if (pg_iter->status == PG_VALID) {
            gc_read_page(ssd, ppa);
            /* delay the maptbl update until "write" happens */
            gc_write_page(ssd, ppa);
            cnt++;
        }
    }

    ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
    
    return cnt;
}

static void mark_line_free(struct ssd *ssd, struct ppa *ppa)
{
    struct line_mgmt *lm = &ssd->lm;
    struct line *line = get_line(ssd, ppa);
    line->ipc = 0;
    line->vpc = 0;
    /* move this line to free line list */
    QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
    lm->free_line_cnt++;
}

static int do_gc(struct ssd *ssd, bool force, NvmeRequest *req)
{
    struct line *victim_line = NULL;
    struct ssdparams *spp = &ssd->sp;
    struct nand_lun *lunp;
    struct ppa ppa;
    int ch, lun;
    
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    int now_ms = now / 1e6;
    int now_s = now / 1e9;

    if (ssd->sp.enable_gc_sync && !force) {//如果开启了gc同步，且盘空闲line未达到高阈值
        // Synchronizing Time Window logic
        int time_window_ms = ssd->sp.gc_sync_window;
        if (ssd->id != (now_ms/time_window_ms) % ssd_id_cnt) {
            if (ssd->sp.straid_debug) {
                ftl_log("GC-FAILED: ssd->id=%d,now_ms=%d,time_window_ms=%d\n", ssd->id, now_ms, time_window_ms);
            }
            return 0;
        }
    }

    if (ssd->sp.group_gc_sync && !force) {//如果开启了group_gc同步，且盘空闲line未达到高阈值
        // Group Synchronizing Time Window logic
        int time_window_s = ssd->sp.gc_sync_window;
        if (ssd->group_id != (now_s/time_window_s) % (SSD_NUM / GROUP_SSD_NUM)) {
            if (ssd->sp.straid_debug) {
                ftl_log("GROUP-GC-FAILED: ssd->group_id=%d,now_s=%d,time_window_s=%d\n", ssd->group_id, now_s, time_window_s);
            }
            return 0;
        }
    }
     

    victim_line = select_victim_line(ssd, force);
    if (!victim_line) {
        return -1;
    }

    ssd->total_gcs++ ;

    ppa.g.blk = victim_line->id;
    ftl_debug("GC-ing line:%d,ipc=%d,victim=%d,full=%d,free=%d\n", ppa.g.blk,
              victim_line->ipc, ssd->lm.victim_line_cnt, ssd->lm.full_line_cnt,
              ssd->lm.free_line_cnt);

    /* copy back valid data */
    for (ch = 0; ch < spp->nchs; ch++) {
        for (lun = 0; lun < spp->luns_per_ch; lun++) {
            ppa.g.ch = ch;
            ppa.g.lun = lun;
            ppa.g.pl = 0;
            lunp = get_lun(ssd, &ppa);
            ssd->num_valid_pages_copied_s += clean_one_block(ssd, &ppa);
            mark_block_free(ssd, &ppa);

            if (spp->enable_gc_delay) {
                struct nand_cmd gce;
                gce.type = GC_IO;
                gce.cmd = NAND_ERASE;
                gce.stime = 0;
                ssd_advance_status(ssd, &ppa, &gce);
            }

            lunp->gc_endtime = lunp->next_lun_avail_time;
            if (lunp->gc_endtime > gc_endtime_array[ssd->id]) {
				gc_endtime_array[ssd->id] = lunp->gc_endtime;
			}
        }
    }

    /* update line status */
    mark_line_free(ssd, &ppa);

    return 0;
}

static uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req)
{
    struct ssdparams *spp = &ssd->sp;
    uint64_t lba = req->slba;
    int nsecs = req->nlb;
    struct ppa ppa;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + nsecs - 1) / spp->secs_per_pg;
    uint64_t lpn;
    uint64_t sublat, maxlat = 0;
    struct nand_lun *lun;
    bool in_gc = false; /* indicate whether any subIO met GC */
    int num_concurrent_gcs = 0;
    /*gql-add for statistic*/

    /*gql- group_gc + ioda reconstruct mod*/
    bool busy_miss = false; //gql-请求盘繁忙且无备份
    bool one_recons = true; //gql- 只有请求盘在GC，其余盘未GC，可以重构该请求

    

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    ssd->total_reads++;

    req->gcrt = 0; //g-gc-remaining time
#define COME_FLAG  1024
#define Back_FLAG1   2048

    // // femu_debug("Ftl-req->usrflag:%llu\n",req->nvm_usrflag);
    // if(req->nvm_usrflag == 1024){
    //     femu_log("Ftl: req->nvme_usrflag:%llu\n",req->nvm_usrflag);
    // }
    // if(req->nvm_usrflag == COME_FLAG){
    //     req->nvm_usrflag = Back_FLAG1;
    // }
    // else {
    //     req->nvm_usrflag = Back_FLAG2;
    // }
#define NVME_NORMAL_REQ (0) //gql-normal request code
#define NVME_RECON_SIG  (1024) //gql-RECONSITUTE SIGNAL
#define NVME_FAILED_REQ  (408) //gql-failed request code
#define NVME_FFAIL_SIG (911)  //gql-启用fast-fail机制
//取出nvm_usrflag的低32位进行判断
    if (get_low32(req->nvm_usrflag) == NVME_FFAIL_SIG && ssd->sp.fast_fail) {//g--fast-fail的io路径
        /* fastfail IO path */
        if(ssd->sp.straid_debug){
            ftl_log("fast-fail-io: req->nvme_usrflag-high32:%lu,low32:%lu,\n",get_high32(req->nvm_usrflag),get_low32(req->nvm_usrflag));
        }
        for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
            ppa = get_maptbl_ent(ssd, lpn);
            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                continue;
            }

            lun = get_lun(ssd, &ppa);
            if (req->stime < lun->gc_endtime) {
                in_gc = true;
                int tgcrt = lun->gc_endtime - req->stime;
                if (req->gcrt < tgcrt) {
                    req->gcrt = tgcrt;
                }
            } else {//g-读请求目标lun未执行gc操作，req->stime >= lun->gc_endtime
                /* NoGC under fastfail path */
                struct nand_cmd srd;
                srd.cmd = NAND_READ;
                srd.stime = req->stime;
                sublat = ssd_advance_status(ssd, &ppa, &srd);
                maxlat = (sublat > maxlat) ? sublat : maxlat;
            }
        }

        if (!in_gc) {//g-如果请求定向设备不在gc中，req->gcrt==0，未被填充
            assert(req->gcrt == 0);
            ssd->reads_nor++ ;//g-normal completed io
            return maxlat;
        }
        /*请求遇到gc，fail掉- 将 NVME_FAILED_REQ填充到req->nvm_usrflag的低32位*/
        req->nvm_usrflag = set_low32(req->nvm_usrflag, NVME_FAILED_REQ);
        if (ssd->sp.straid_debug) {
            ftl_log("FAILED IO : req->nvme_usrflag-high32:%lu,low32:%lu,\n",get_high32(req->nvm_usrflag),get_low32(req->nvm_usrflag));
        }

        ssd->reads_block++;// blocked by first time

        //how many ssds are in gc when a filed io ocurs
        for (int i = 0; i < ssd_id_cnt; i++) {
            if (req->stime < gc_endtime_array[i]) {
                num_concurrent_gcs++;
            }
        }
        ssd->num_reads_blocked_by_gc[num_concurrent_gcs]++;

        return 0;
    } else {
        ssd->total_read_num++;
        //ftl_log("normal-io: req->nvme_usrflag-high32:%lu,low32:%lu,\n",get_high32(req->nvm_usrflag),get_low32(req->nvm_usrflag));
        int max_gcrt = 0;
        bool sub_gc = false;
        /* normal IO read path */
        for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
            ppa = get_maptbl_ent(ssd, lpn);
            if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
                //printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
                //printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
                //ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
                continue;
            }

            lun = get_lun(ssd, &ppa);
            //根据定向lun是否在gc，选择用哪种方式来执行本次读请求
            if (ssd->sp.buffer_read)
            {
                if (req->stime >= lun->gc_endtime) {//g-读请求目标lun未执行gc操作，req->stime >= lun->gc_endtime
                    sublat = Idle_buffer_read(ssd,ssd,lpn,req);    
                } else {
                    sublat = busy_buffer_read(ssd,ssd,lpn,req);
                    
                    if (ssd->sp.grpgc_recon){
                        if (sublat > NAND_READ_LATENCY){                   
                            busy_miss = true;
                        }
                    }
                }
                ssd->total_read_pages++;

                
            }
            else {
                if (req->stime < lun->gc_endtime) {
                    sub_gc = true;
                }                   
                if (req->stime < lun->gc_endtime && max_gcrt < lun->gc_endtime - req->stime) {
                        max_gcrt = lun->gc_endtime - req->stime;
                }

                struct nand_cmd srd;
                srd.type = USER_IO;
                srd.cmd = NAND_READ;
                srd.stime = req->stime;
                sublat = ssd_advance_status(ssd, &ppa, &srd);
            }

            maxlat = (sublat > maxlat) ? sublat : maxlat;   
        }
        if(ssd->sp.grpgc_recon)
        {
            int this_ssd_id = ssd->id;
            for (int i=0; i < ssd_id_cnt; i++)
            {
                if (i == this_ssd_id){
                    continue;
                }
                if (req->stime < gc_endtime_array[i]){
                    one_recons = false;
                    break;
                }
            }
        }

        if (ssd->sp.buffer_read && ssd->sp.grpgc_recon && one_recons && busy_miss){
            /*gql-结合ioda的降低读的方案去fast-fail掉请求，实现重构*/
            req->nvm_usrflag = set_low32(req->nvm_usrflag, NVME_FAILED_REQ);
            return 0;
        }
        else 
        {
            if(sub_gc){//blocked read in normal io path
                ssd->block_read_num++;
                for (int i = 0; i < ssd_id_cnt; i++) {
                    if (req->stime < gc_endtime_array[i]) {
                        num_concurrent_gcs++;
                    }
                }
                ssd->num_reads_blocked_by_gc[num_concurrent_gcs]++;
            }

            if (ssd->sp.fast_fail)
            {
                if (max_gcrt > 0){
                    ssd->reads_reblk++;//reconstruct read io blocked by gc for the second time
                }      
                ssd->reads_recon++;  //reconstruct read io finished normally
            }


            return maxlat;
        }
    }
}

static uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req)
{
    uint64_t lba = req->slba;
    struct ssdparams *spp = &ssd->sp;
    int len = req->nlb;
    uint64_t start_lpn = lba / spp->secs_per_pg;
    uint64_t end_lpn = (lba + len - 1) / spp->secs_per_pg;
    struct ppa ppa;
    uint64_t lpn;
    uint64_t curlat = 0, maxlat = 0;
    int r;

    if (end_lpn >= spp->tt_pgs) {
        ftl_err("start_lpn=%"PRIu64",tt_pgs=%d\n", start_lpn, ssd->sp.tt_pgs);
    }

    while (should_gc_high(ssd)) {
        /* perform GC here until !should_gc(ssd) */
        r = do_gc(ssd, true, NULL);
        if (r == -1)
            break;
    }

    for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
        ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa)) {
            /* update old page information first */
            mark_page_invalid(ssd, &ppa);
            set_rmap_ent(ssd, INVALID_LPN, &ppa);
        }

        if (ssd->sp.buffer_read)
        {
            delete_from_buffer(ssd,lpn);
        }
        /* new write */
        ppa = get_new_page(ssd);
        /* update maptbl */
        set_maptbl_ent(ssd, lpn, &ppa);
        /* update rmap */
        set_rmap_ent(ssd, lpn, &ppa);

        mark_page_valid(ssd, &ppa);

        /* need to advance the write pointer here */
        ssd_advance_write_pointer(ssd);

        struct nand_cmd swr;
        swr.type = USER_IO;
        swr.cmd = NAND_WRITE;
        swr.stime = req->stime;
        /* get latency statistics */
        curlat = ssd_advance_status(ssd, &ppa, &swr);
        maxlat = (curlat > maxlat) ? curlat : maxlat;
    }

    return maxlat;
}

static void *ftl_thread(void *arg)
{
    FemuCtrl *n = (FemuCtrl *)arg;
    struct ssd *ssd = n->ssd;
    NvmeRequest *req = NULL;
    uint64_t lat = 0;
    int rc;
    int i;

    while (!*(ssd->dataplane_started_ptr)) {
        usleep(100000);
    }

    /* FIXME: not safe, to handle ->to_ftl and ->to_poller gracefully */
    ssd->to_ftl = n->to_ftl;
    ssd->to_poller = n->to_poller;

    while (1) {
        for (i = 1; i <= n->num_poller; i++) {
            if (!ssd->to_ftl[i] || !femu_ring_count(ssd->to_ftl[i]))
                continue;

            rc = femu_ring_dequeue(ssd->to_ftl[i], (void *)&req, 1);
            if (rc != 1) {
                printf("FEMU: FTL to_ftl dequeue failed\n");
            }

            ftl_assert(req);
            switch (req->cmd.opcode) {
            case NVME_CMD_WRITE:
                lat = ssd_write(ssd, req);
                break;
            case NVME_CMD_READ:
                lat = ssd_read(ssd, req);
                // if (lat > 1e9) {
                //     printf("FEMU: Read latency is > 1s, what's going on!\n");
                // }
                break;
            case NVME_CMD_DSM:
                lat = 0;
                break;
            default:
                //ftl_err("FTL received unkown request type, ERROR\n");
                ;
            }

            req->reqlat = lat;
            req->expire_time += lat;

            rc = femu_ring_enqueue(ssd->to_poller[i], (void *)&req, 1);
            if (rc != 1) {
                ftl_err("FTL to_poller enqueue failed\n");
            }

            /* clean one line if needed (in the background) */
            if (should_gc(ssd)) {
                do_gc(ssd, false, req);
            }
        }
    }

    return NULL;
}


static void ssd_remove_node(struct ssd *target_ssd,tAVLTree *buffer)
{
    uint64_t evicted_lpn = buffer->buffer_tail->key;
    struct ppa ppa = get_maptbl_ent(target_ssd, evicted_lpn);
    if (mapped_ppa(&ppa)) {
        /* update old page information first */
        mark_page_invalid(target_ssd, &ppa);
        set_rmap_ent(target_ssd, INVALID_LPN, &ppa);
    }
}

static uint64_t Idle_buffer_read(struct ssd *ssd, struct ssd *target_ssd, uint64_t lpn, NvmeRequest *req)
{
    uint64_t lat = 0;
    struct ppa ppa = get_maptbl_ent(ssd, lpn);
    if (mapped_ppa(&ppa) && valid_ppa(ssd, &ppa)) {
        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        lat += ssd_advance_status(ssd, &ppa, &srd);
    }
    tAVLTree *buffer = ssd->dram.buffer;
    struct buffer_group * node = buffer_search(buffer, lpn);
    if (node == NULL)
    {
        tAVLTree *L_buffer = ssd->dram.L_buffer;
        struct buffer_group * L_node = buffer_search(L_buffer, lpn);
        if (L_node == NULL)//Lbuffer miss，不满足进入缓存区的条件
        {
            if(!L_buffer->buffer_full_flag)
            {
                create_new_bufnode(L_buffer, lpn);
            }
            else
            {
                dram_delete_buffer_node(L_buffer);
                create_new_bufnode(L_buffer, lpn);
            }

        }
        else //Lbuffer hit，识别为真热读数据，进入缓冲区
        {

            if (!buffer->buffer_full_flag)
            {
                create_new_bufnode(buffer, lpn);
            }
            else
            {
                //ssd_remove_node(target_ssd, buffer);/* mark the backup data invalid*/
                dram_delete_buffer_node(buffer);
                create_new_bufnode(buffer, lpn);
            }

            //write_to_buffer(ssd, target_ssd, lpn, req);/* write the read data to the backup space of another ssd */
        }
    }
    else
    {
        LRU_Tofirst(buffer, node);
    }
    return lat;
}

static uint64_t busy_buffer_read(struct ssd *ssd, struct ssd *target_ssd, uint64_t lpn, NvmeRequest *req)
{
    uint64_t lat = 0;
    tAVLTree *buffer = ssd->dram.buffer;
    struct buffer_group * node = buffer_search(buffer, lpn);
    if (node == NULL)
    {
        struct ppa ppa = get_maptbl_ent(ssd, lpn);
        if (mapped_ppa(&ppa) && valid_ppa(ssd, &ppa)) {
            struct nand_cmd srd;
            srd.type = USER_IO;
            srd.cmd = NAND_READ;
            srd.stime = req->stime;
            lat += ssd_advance_status(ssd, &ppa, &srd);
        }

        tAVLTree *L_buffer = ssd->dram.L_buffer;
        struct buffer_group * L_node = buffer_search(L_buffer, lpn);
        if (L_node == NULL)//Lbuffer miss，不满足进入缓存区的条件
        {
            if(!L_buffer->buffer_full_flag)
            {
                create_new_bufnode(L_buffer, lpn);
            }
            else
            {
                dram_delete_buffer_node(L_buffer);
                create_new_bufnode(L_buffer, lpn);
            }

        }
        else //Lbuffer hit，识别为真热读数据，进入缓冲区
        {
            if (!buffer->buffer_full_flag)
            {
                create_new_bufnode(buffer, lpn);
            }
            else
            {
                //ssd_remove_node(target_ssd, buffer);/* mark the backup data invalid*/
                dram_delete_buffer_node(buffer);
                create_new_bufnode(buffer, lpn);
            }

            //write_to_buffer(ssd, target_ssd, lpn, req);/* write the read data to the backup space of another ssd */
        }
    }
    else
    {
        // lat += read_from_buffer(ssd, target_ssd, lpn, req);/* read the buffered data*/
        if(req->stime < gc_endtime_array[ssd->bk_id]){ //gql-buffer bage cannot respond for backup ssd is in gc
            ssd->buffer_block_pages++;
        }
        if (req->stime < gc_endtime_array[ssd->bk_id] && ssd->sp.gc_streering)
        {
            lat += gc_endtime_array[ssd->bk_id] - req->stime;
        }
        else {
            lat += NAND_READ_LATENCY;
        }
        LRU_Tofirst(buffer, node);
        ssd->buffer_page_num++;
    }
    return lat;
}

static int delete_from_buffer(struct ssd *ssd, uint64_t lpn)
{
    tAVLTree *buffer = ssd->dram.buffer;
    struct buffer_group * node = buffer_search(buffer, lpn);
    if (node != NULL)
    {
        //ssd_remove_node(target_ssd, buffer);/* mark the backup data invalid*/
        dram_delete_buffer_node(buffer);
        return 1;
    }
    return 0;
}

// uint64_t handle_buffer_read(struct ssd *ssd, struct ssd *target_ssd , uint64_t lpn, NvmeRequest *req)
// {
//     uint64_t lat = 0;

//     tAVLTree *buffer = ssd->dram.buffer;

//     struct buffer_group * node = buffer_search(buffer, lpn);

//     if (node == NULL) {
//         if (!buffer->buffer_full_flag)
//         {
//             create_new_bufnode(buffer, lpn);
//         }
//         else 
//         {
//             /*need  to mark the evcited node invalid for gc to clean it*/
//             ssd_remove_node(target_ssd, buffer);//multi-thread-mark_page_invalid
//             uint64_t evicted_lpn = buffer->buffer_tail->key;
//             dram_delete_buffer_node(buffer);
//             create_new_bufnode(buffer, lpn);
//         }

//         // read from SSD ,then return this latency
//         //after this ,should write the data to the target ssd , ignore the latency(no really request here)。
//         lat += read_from_ssd(ssd, target_ssd, lpn, req);
//         //处理lpn到一个盘的op空间的ppn中去，并且保证查找的时候lpn对应的LUN的id为同一个，模拟在备份中读数据的GC干扰性
//         //multi-thread-maptal。
//         write_to_buffer(ssd, target_ssd, lpn, req);//actually, buffer means the backup space of another ssd.
//         return (lat);
//     }
//     else {

//         lat += read_from_buffer(ssd, target_ssd, lpn, req);
        
//         LRU_Tofirst(buffer, node);

//         return lat;// read from ssd ,depends on thr real letency not the DRAM_READ_LATENCY
//     }


// }

uint64_t read_from_ssd(struct ssd *ssd, struct ssd *target_ssd , uint64_t lpn, NvmeRequest *req)
{
    uint64_t lat=0;
    struct ppa ppa = get_maptbl_ent(ssd, lpn);
    if (mapped_ppa(&ppa) && valid_ppa(ssd, &ppa)) {
        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        lat += ssd_advance_status(ssd, &ppa, &srd);
    }
    return lat;
}

uint64_t write_to_buffer(struct ssd *ssd, struct ssd * target_ssd , uint64_t lpn, NvmeRequest *req)
{
    // need to change ,the lpn cannot use get_maptbl_ent to get the ppa ,its backup space ,avoid using the user space.
    uint64_t lat = 0;
    uint64_t nlpn = lpn_trans(lpn);/*Map the lpn to 1GB backup space*/
    struct ppa ppa = get_maptbl_ent(target_ssd, nlpn);
    if (mapped_ppa(&ppa)) {
        /* update old page information first */
        mark_page_invalid(target_ssd, &ppa);
        set_rmap_ent(target_ssd, INVALID_LPN, &ppa);
    }
    /* new write */
    ppa = get_new_page(target_ssd);
    /* update maptbl */
    set_maptbl_ent(target_ssd, lpn, &ppa);
    /* update rmap */
    set_rmap_ent(target_ssd, lpn, &ppa);

    mark_page_valid(target_ssd, &ppa);

    /* need to advance the write pointer here */
    ssd_advance_write_pointer(target_ssd);

    struct nand_cmd swr;
    swr.type = USER_IO;
    swr.cmd = NAND_WRITE;
    swr.stime = req->stime;
    /* get latency statistics */
    lat += ssd_advance_status(target_ssd, &ppa, &swr);

    return lat;
}

uint64_t read_from_buffer(struct ssd *ssd, struct ssd * target_ssd , uint64_t lpn, NvmeRequest *req)
{
    uint64_t lat=0;
    uint64_t nlpn = lpn_trans(lpn);
    struct ppa ppa = get_maptbl_ent(target_ssd, nlpn);
    if (mapped_ppa(&ppa) && valid_ppa(ssd, &ppa)) {
        struct nand_cmd srd;
        srd.type = USER_IO;
        srd.cmd = NAND_READ;
        srd.stime = req->stime;
        lat += ssd_advance_status(target_ssd, &ppa, &srd);
    }
    return lat;   
}