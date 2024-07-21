#include "../nvme.h"
#include "./ftl.h"

static void bb_init_ctrl_str(FemuCtrl *n)
{
    static int fsid_vbb = 0;
    const char *vbbssd_mn = "FEMU BlackBox-SSD Controller";
    const char *vbbssd_sn = "vSSD";

    nvme_set_ctrl_name(n, vbbssd_mn, vbbssd_sn, &fsid_vbb);
}

/* bb <=> black-box */
static void bb_init(FemuCtrl *n, Error **errp)
{
    struct ssd *ssd = n->ssd = g_malloc0(sizeof(struct ssd));

    bb_init_ctrl_str(n);

    ssd->dataplane_started_ptr = &n->dataplane_started;
    ssd->ssdname = (char *)n->devname;
    femu_debug("Starting FEMU in Blackbox-SSD mode ...\n");
    ssd_init(n);
}

static void bb_flip(FemuCtrl *n, NvmeCmd *cmd)
{
    struct ssd *ssd = n->ssd;
    int64_t cdw10 = le64_to_cpu(cmd->cdw10);

    switch (cdw10) {
    case FEMU_ENABLE_GC_DELAY:
        ssd->sp.enable_gc_delay = true;
        femu_log("%s,FEMU GC Delay Emulation [Enabled]!\n", n->devname);
        break;
    case FEMU_DISABLE_GC_DELAY:
        ssd->sp.enable_gc_delay = false;
        femu_log("%s,FEMU GC Delay Emulation [Disabled]!\n", n->devname);
        break;
    case FEMU_ENABLE_DELAY_EMU:
        ssd->sp.pg_rd_lat = NAND_READ_LATENCY;
        ssd->sp.pg_wr_lat = NAND_PROG_LATENCY;
        ssd->sp.blk_er_lat = NAND_ERASE_LATENCY;
        ssd->sp.ch_xfer_lat = 0;
        femu_log("%s,FEMU Delay Emulation [Enabled]!\n", n->devname);
        break;
    case FEMU_DISABLE_DELAY_EMU:
        ssd->sp.pg_rd_lat = 0;
        ssd->sp.pg_wr_lat = 0;
        ssd->sp.blk_er_lat = 0;
        ssd->sp.ch_xfer_lat = 0;
        femu_log("%s,FEMU Delay Emulation [Disabled]!\n", n->devname);
        break;
    case FEMU_RESET_ACCT:
        n->nr_tt_ios = 0;
        n->nr_tt_late_ios = 0;
        femu_log("%s,Reset tt_late_ios/tt_ios,%lu/%lu\n", n->devname,
                n->nr_tt_late_ios, n->nr_tt_ios);
        break;
    case FEMU_ENABLE_LOG:
        n->print_log = true;
        femu_log("%s,Log print [Enabled]!\n", n->devname);
        break;
    case FEMU_DISABLE_LOG:
        n->print_log = false;
        femu_log("%s,Log print [Disabled]!\n", n->devname);
        break;
    
    /*gql- gc-control switch */
    case FEMU_SYNC_GC:
        ssd->sp.enable_gc_sync = true;
        femu_log("%s,FEMU GC Synchronization [Enabled]!\n", n->devname);
        break;
    case FEMU_UNSYNC_GC:
        ssd->sp.enable_gc_sync = false;
        femu_log("%s,FEMU GC Synchronization [Disabled]!\n", n->devname);
        break;

    case FEMU_WINDOW_100MS:
        femu_log("FEMU_WINDOW_100MS\n");
        ssd->sp.gc_sync_window = 100;
        break;

    case FEMU_FAST_FAIL_SWITCH:
        ssd->sp.fast_fail = 1 - ssd->sp.fast_fail ;
        femu_log("%s,FEMU Fast-Fail value--(1)for on ,(0)for off:  %d\n", n->devname, ssd->sp.fast_fail);
        break;

    case FEMU_STRAID_DEBUG_SWITCH:
        ssd->sp.straid_debug = 1- ssd->sp.straid_debug;
        femu_log("%s,FEMU Straid-Debug value--(1)for on ,(0)for off:  %d\n", n->devname, ssd->sp.straid_debug);
        break;
    
    case NORMAL_MOD:
        ssd->sp.buffer_read = 0;
        ssd->sp.group_gc_sync = 0;
        ssd->sp.gc_streering = 0;
        ssd->sp.grpgc_recon = 0;
        ssd->sp.enable_gc_sync = false;
        ssd->sp.fast_fail = 0;
        femu_log("%s, gc_sync, fast_fail, buffer_read ,group_gc: off \n",
                n->devname);
        break;
    
    case RECONS_MOD:
        ssd->sp.buffer_read = 0;
        ssd->sp.group_gc_sync = 0;
        ssd->sp.gc_streering = 0;
        ssd->sp.grpgc_recon = 0;
        ssd->sp.enable_gc_sync = true;
        ssd->sp.fast_fail = 1;
        femu_log("%s, gc_sync: on, fast_fail: on, buffer_read: off \n",
                n->devname);
        break;

    case BUFFER_MOD:
        ssd->sp.buffer_read = 1;
        ssd->sp.group_gc_sync = 1;
        ssd->sp.gc_streering = 0;
        ssd->sp.grpgc_recon = 0;
        ssd->sp.enable_gc_sync = false;
        ssd->sp.fast_fail = 0;
        femu_log("%s, buffer_read: on ,group_gc: on, gc_steering: off \n",
                n->devname);
        break;

    case GROUP_GC_SWITCH:
        ssd->sp.group_gc_sync = 1 - ssd->sp.group_gc_sync;
        femu_log("%s,FEMU Group-GC-SYNC value--(1)for on ,(0)for off:  %d\n", n->devname, ssd->sp.group_gc_sync);
        break;

    case SHOW_GROUP_INFO:
        femu_log("%s,GROUP INFO: GROUP_ID: %d,BK_SSD_ID: %d, GROUP_NUM: %d,\n", 
            n->devname,
            ssd->group_id,
            ssd->bk_id,
            (SSD_NUM / GROUP_SSD_NUM));
        break;
    
    case GC_STEERING_MOD:
        ssd->sp.buffer_read = 1;
        ssd->sp.group_gc_sync = 0;
        ssd->sp.gc_streering = 1;
        ssd->sp.grpgc_recon = 0;
        ssd->sp.enable_gc_sync = false;
        ssd->sp.fast_fail = 0;
        femu_log("%s, buffer_read: on ,group_gc: off, gc_steering: on  \n",
                n->devname);
        break;

    case GRPGC_RECON_MOD:
        ssd->sp.buffer_read = 1;
        ssd->sp.group_gc_sync = 1;
        ssd->sp.grpgc_recon = 1;
        ssd->sp.gc_streering = 0;
        ssd->sp.enable_gc_sync = false;
        ssd->sp.fast_fail = 0;
        femu_log("%s,buffer_read: on ,group_gc: on, grpgc_recon: on \n",
                n->devname);
        break;

    case BACKUP_SPACE_ADD:
        ssd->dram.dram_capacity += 20*1024*1024;
        update_buffer_page(ssd->dram.buffer , ssd->dram.dram_capacity);
        femu_log("%s, Backup Space : %d M !\n", n->devname,(int)ssd->dram.dram_capacity/(1024*1024));
        break;

    case BACKUP_SPACE_DEL:
        ssd->dram.dram_capacity -= 20*1024*1024;
        update_buffer_page(ssd->dram.buffer , ssd->dram.dram_capacity);
        femu_log("%s, Backup Space : %d M !\n", n->devname,(int)ssd->dram.dram_capacity/(1024*1024));
        break;

    
    case FEMU_NAND_UTILIZATION_LOG:
        ssd->nand_utilization_log = 1 - ssd->nand_utilization_log;
        femu_log("%s, FEMU_NAND_UTILIZATION_LOG TOGGLE, current value: %d\n",
                n->devname, ssd->nand_utilization_log);
        break;

    case FEMU_PRINT_CONFIG:
        femu_log("%s,Config: SYNC_gc: %d, F_FAIL: %d, BUF_R: %d, GRP_GC: %d,STEERING: %d,grpgc_recon: %d,cache: %d M\n", 
                    n->devname,
                    ssd->sp.enable_gc_sync,
                    ssd->sp.fast_fail,
                    ssd->sp.buffer_read,
                    ssd->sp.group_gc_sync,
                    ssd->sp.gc_streering,
                    ssd->sp.grpgc_recon,
                    (int)ssd->dram.dram_capacity/(1024*1024));
        break;

    case FEMU_PRINT_AND_RESET_COUNTERS:
        femu_log("FEMU_PRINT_AND_RESET_COUNTERS\n");
        printf("SSD%d:", ssd->id);
        for (int i = 0; i <= SSD_NUM; i++) {
            printf(" %dGC %d,", i, ssd->num_reads_blocked_by_gc[i]);
        }
        printf("\n");
        // for (int i = 0; i <= SSD_NUM; i++) {
        //     printf(" %dGC %.4f, ", i, (float)ssd->num_reads_blocked_by_gc[i] * 100 / ssd->total_reads);
        // }
        // printf("\n");

        if (ssd->sp.fast_fail && ssd->sp.enable_gc_sync){//IODA_GC-mod
            printf("total_gc:%d ,  total_reads:%d\n", ssd->total_gcs,ssd->total_reads );
            printf("total_nor:%d ,total_nor:%d ,  total_block:%d ,  total_recon:%d ,  total_rebl:%d\n",ssd->total_reads, ssd->reads_nor,ssd->reads_block,ssd->reads_recon,ssd->reads_reblk);
        } 
        else if (ssd->sp.buffer_read){//GRUOP_GC-mod
            printf("total_gc:%d ,  total_reads:%d\n", ssd->total_gcs,ssd->total_reads );
            printf("read_num:%d ,  read_pages:%d ,  buffer_pages:%d ,  buffer_block:%d\n",ssd->total_read_num, ssd->total_read_pages, ssd->buffer_page_num, ssd->buffer_block_pages);
        }
        else{//BASE-mod
            printf("total_gc:%d ,  total_reads:%d\n", ssd->total_gcs,ssd->total_reads );
            printf("total_reads:%d ,  block_reads:%d\n", ssd->total_read_num,ssd->block_read_num );
        }


        //gql-清除数据
        for (int i = 0; i <= SSD_NUM; i++) {
            ssd->num_reads_blocked_by_gc[i] = 0;
        }
        //gql-重置gcs，reads for block nor recon rebl
        ssd->total_reads = 0;
        ssd->total_gcs = 0;
        ssd->reads_block = 0;
        ssd->reads_nor = 0;
        ssd->reads_recon = 0;
        ssd->reads_reblk = 0;

        ssd->total_read_num = 0;
        ssd->total_read_pages = 0;
        ssd->block_read_num = 0;
        ssd->buffer_page_num = 0;
        ssd->buffer_block_pages = 0;
        break;
    


    default:
        printf("FEMU:%s,Not implemented flip cmd (%lu)\n", n->devname, cdw10);
    }
}

static uint16_t bb_nvme_rw(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                           NvmeRequest *req)
{
    return nvme_rw(n, ns, cmd, req);
}

static uint16_t bb_io_cmd(FemuCtrl *n, NvmeNamespace *ns, NvmeCmd *cmd,
                          NvmeRequest *req)
{
    switch (cmd->opcode) {
    case NVME_CMD_READ:
    case NVME_CMD_WRITE:
        return bb_nvme_rw(n, ns, cmd, req);
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

static uint16_t bb_admin_cmd(FemuCtrl *n, NvmeCmd *cmd)
{
    switch (cmd->opcode) {
    case NVME_ADM_CMD_FEMU_FLIP:
        bb_flip(n, cmd);
        return NVME_SUCCESS;
    default:
        return NVME_INVALID_OPCODE | NVME_DNR;
    }
}

int nvme_register_bbssd(FemuCtrl *n)
{
    n->ext_ops = (FemuExtCtrlOps) {
        .state            = NULL,
        .init             = bb_init,
        .exit             = NULL,
        .rw_check_req     = NULL,
        .admin_cmd        = bb_admin_cmd,
        .io_cmd           = bb_io_cmd,
        .get_log          = NULL,
    };

    return 0;
}

