/*
 * Copyright IBM Corp. 2007
 *
 * Authors:
 *  Dan Smith <danms@us.ibm.com>
 *  Jay Gagnon <grendel@linux.vnet.ibm.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/vfs.h>
#include <errno.h>
#include <uuid/uuid.h>

#include <libvirt/libvirt.h>

#include "config.h"

#include "cmpidt.h"
#include "cmpift.h"
#include "cmpimacs.h"

#include <libcmpiutil/libcmpiutil.h>
#include "misc_util.h"
#include <libcmpiutil/std_association.h>
#include "device_parsing.h"
#include "svpc_types.h"

#include "Virt_SettingsDefineCapabilities.h"
#include "Virt_DevicePool.h"
#include "Virt_RASD.h"
#include "Virt_VSMigrationCapabilities.h"
#include "Virt_VSMigrationSettingData.h"

const static CMPIBroker *_BROKER;

/* These are used in more than one place so they are defined here. */
#define SDC_DISK_MIN 2000
#define SDC_DISK_DEF 5000
#define SDC_DISK_INC 250

static bool system_has_vt(virConnectPtr conn)
{
        char *caps = NULL;
        bool vt = false;

        caps = virConnectGetCapabilities(conn);
        if (caps != NULL)
                vt = (strstr(caps, "hvm") != NULL);

        free(caps);

        return vt;
}

static CMPIInstance *default_vssd_instance(const char *prefix,
                                           const char *ns)
{
        CMPIInstance *inst = NULL;
        uuid_t uuid;
        char uuidstr[37];
        char *iid = NULL;

        uuid_generate(uuid);
        uuid_unparse(uuid, uuidstr);

        if (asprintf(&iid, "%s:%s", prefix, uuidstr) == -1) {
                CU_DEBUG("Failed to generate InstanceID string");
                goto out;
        }

        inst = get_typed_instance(_BROKER,
                                  prefix,
                                  "VirtualSystemSettingData",
                                  ns);
        if (inst == NULL) {
                CU_DEBUG("Failed to create default VSSD instance");
                goto out;
        }

        CMSetProperty(inst, "InstanceID",
                      (CMPIValue *)iid, CMPI_chars);

 out:
        free(iid);

        return inst;
}

static CMPIInstance *_xen_base_vssd(virConnectPtr conn,
                                    const char *ns,
                                    const char *name)
{
        CMPIInstance *inst;

        inst = default_vssd_instance(pfx_from_conn(conn), ns);
        if (inst == NULL)
                return NULL;

        CMSetProperty(inst, "VirtualSystemIdentifier",
                      (CMPIValue *)name, CMPI_chars);

        return inst;
}

static CMPIStatus _xen_vsmc_to_vssd(virConnectPtr conn,
                                    const char *ns,
                                    struct inst_list *list)
{
        CMPIStatus s = {CMPI_RC_OK, NULL};
        CMPIInstance *inst;
        int isfv = 0;

        inst = _xen_base_vssd(conn, ns, "Xen_Paravirt_Guest");
        if (inst == NULL)
                goto error;

        CMSetProperty(inst, "Bootloader",
                      (CMPIValue *)"/usr/bin/pygrub", CMPI_chars);

        CMSetProperty(inst, "isFullVirt",
                      (CMPIValue *)&isfv, CMPI_boolean);

        inst_list_add(list, inst);

        if (system_has_vt(conn)) {
                isfv = 1;

                inst = _xen_base_vssd(conn, ns, "Xen_Fullvirt_Guest");
                if (inst == NULL)
                        goto error;

                CMSetProperty(inst, "BootDevice",
                              (CMPIValue *)"hda", CMPI_chars);

                CMSetProperty(inst, "isFullVirt",
                              (CMPIValue *)&isfv, CMPI_boolean);

                inst_list_add(list, inst);
        }

        return s;

 error:
        cu_statusf(_BROKER, &s,
                   CMPI_RC_ERR_FAILED,
                   "Unable to create %s_VSSD instance",
                   pfx_from_conn(conn));

        return s;
}

static CMPIStatus _kvm_vsmc_to_vssd(virConnectPtr conn,
                                    const char *ns,
                                    struct inst_list *list)
{
        CMPIStatus s = {CMPI_RC_OK, NULL};
        CMPIInstance *inst;

        inst = default_vssd_instance(pfx_from_conn(conn), ns);
        if (inst == NULL) {
                cu_statusf(_BROKER, &s,
                           CMPI_RC_ERR_FAILED,
                           "Unable to create %s_VSSD instance",
                           pfx_from_conn(conn));
                goto out;
        }

        CMSetProperty(inst, "VirtualSystemIdentifier",
                      (CMPIValue *)"KVM_guest", CMPI_chars);

        CMSetProperty(inst, "BootDevice",
                      (CMPIValue *)"hda", CMPI_chars);

        inst_list_add(list, inst);
 out:
        return s;
}

static CMPIStatus _lxc_vsmc_to_vssd(virConnectPtr conn,
                                    const char *ns,
                                    struct inst_list *list)
{
        CMPIStatus s = {CMPI_RC_OK, NULL};
        CMPIInstance *inst;

        inst = default_vssd_instance(pfx_from_conn(conn), ns);
        if (inst == NULL) {
                cu_statusf(_BROKER, &s,
                           CMPI_RC_ERR_FAILED,
                           "Unable to create %s_VSSD instance",
                           pfx_from_conn(conn));
                goto out;
        }

        CMSetProperty(inst, "InitPath",
                      (CMPIValue *)"/sbin/init", CMPI_chars);

        inst_list_add(list, inst);
 out:
        return s;
}

static CMPIStatus vsmc_to_vssd(const CMPIObjectPath *ref,
                               struct std_assoc_info *info,
                               struct inst_list *list)
{
        CMPIStatus s;
        virConnectPtr conn = NULL;
        const char *cn;
        const char *ns;

        cn = CLASSNAME(ref);
        ns = NAMESPACE(ref);

        conn = connect_by_classname(_BROKER, cn, &s);
        if (conn == NULL)
                goto out;

        if (STARTS_WITH(cn, "Xen"))
                s = _xen_vsmc_to_vssd(conn, ns, list);
        else if (STARTS_WITH(cn, "KVM"))
                s = _kvm_vsmc_to_vssd(conn, ns, list);
        else if (STARTS_WITH(cn, "LXC"))
                s = _lxc_vsmc_to_vssd(conn, ns, list);
        else
                cu_statusf(_BROKER, &s,
                           CMPI_RC_ERR_FAILED,
                           "Invalid reference");

 out:
        virConnectClose(conn);

        return s;
}

static bool rasd_prop_copy_value(struct sdc_rasd_prop src, 
                                 struct sdc_rasd_prop *dest)
{
        bool rc = true;

        CU_DEBUG("Copying '%s'", src.field);
        if (src.type & CMPI_string) {
                dest->value = (CMPIValue *)strdup((char *)src.value);
        } else if (src.type & CMPI_INTEGER) {
                dest->value = malloc(sizeof(CMPIValue));
                memcpy(dest->value, src.value, sizeof(CMPIValue));
        } else {
                rc = false;
        }

        return rc;
}

static bool dup_rasd_prop_list(struct sdc_rasd_prop *src, 
                               struct sdc_rasd_prop **dest)
{
        int count, i;
        bool ret;
        *dest = NULL;
        
        for (i = 0, count = 1; src[i].field != NULL; i++, count++) {
                *dest = realloc(*dest, count * sizeof(struct sdc_rasd_prop));
                (*dest)[i].field = strdup(src[i].field);
                ret = rasd_prop_copy_value(src[i], &(*dest)[i]);
                (*dest)[i].type = src[i].type;
        }
        
        /* Make sure to terminate the list. */
        *dest = realloc(*dest, count * sizeof(struct sdc_rasd_prop));
        (*dest)[i] = (struct sdc_rasd_prop)PROP_END;

        return true;
}

static void free_rasd_prop_list(struct sdc_rasd_prop *prop_list)
{
        int i;
        
        if (!prop_list)
                return;

        for (i = 0; prop_list[i].field != NULL; i++) {
                free(prop_list[i].field);
                free(prop_list[i].value);
        }
        
        free (prop_list);
}

static struct sdc_rasd_prop *mem_max(const CMPIObjectPath *ref,
                                     CMPIStatus *s)
{
        bool ret;
        struct sdc_rasd_prop *rasd = NULL;
        uint64_t max_vq = MAX_MEM;

        struct sdc_rasd_prop tmp[] = {
                {"InstanceID", (CMPIValue *)"Maximum", CMPI_chars},
                {"AllocationUnits", (CMPIValue *)"KiloBytes", CMPI_chars},
                {"VirtualQuantity", (CMPIValue *)&max_vq, CMPI_uint64},
                PROP_END
        };
        
        ret = dup_rasd_prop_list(tmp, &rasd);
        if (!ret) {
                cu_statusf(_BROKER, s, 
                           CMPI_RC_ERR_FAILED,
                           "Could not copy RASD");
        }

        return rasd;
}

static struct sdc_rasd_prop *mem_min(const CMPIObjectPath *ref,
                                     CMPIStatus *s)
{
        bool ret;
        struct sdc_rasd_prop *rasd = NULL;
        uint64_t min_vq = 64 << 10;

        struct sdc_rasd_prop tmp[] = {
                {"InstanceID", (CMPIValue *)"Minimum", CMPI_chars},
                {"AllocationUnits", (CMPIValue *)"KiloBytes", CMPI_chars},
                {"VirtualQuantity", (CMPIValue *)&min_vq, CMPI_uint64},
                PROP_END
        };

        ret = dup_rasd_prop_list(tmp, &rasd);
        if (!ret) {
                cu_statusf(_BROKER, s, 
                           CMPI_RC_ERR_FAILED,
                           "Could not copy RASD");
        }

        return rasd;
}

static struct sdc_rasd_prop *mem_def(const CMPIObjectPath *ref,
                                     CMPIStatus *s)
{
        bool ret;
        struct sdc_rasd_prop *rasd = NULL;
        uint64_t def_vq = 256 << 10;

        struct sdc_rasd_prop tmp[] = {
                {"InstanceID", (CMPIValue *)"Default", CMPI_chars},
                {"AllocationUnits", (CMPIValue *)"KiloBytes", CMPI_chars},
                {"VirtualQuantity", (CMPIValue *)&def_vq, CMPI_uint64},
                PROP_END
        };

        ret = dup_rasd_prop_list(tmp, &rasd);
        if (!ret) {
                cu_statusf(_BROKER, s, 
                           CMPI_RC_ERR_FAILED,
                           "Could not copy RASD");
        }

        return rasd;
}

static struct sdc_rasd_prop *mem_inc(const CMPIObjectPath *ref,
                                     CMPIStatus *s)
{
        bool ret;
        struct sdc_rasd_prop *rasd = NULL;
        uint64_t inc_vq = 1 << 10;

        struct sdc_rasd_prop tmp[] = {
                {"InstanceID", (CMPIValue *)"Increment", CMPI_chars},
                {"AllocationUnits", (CMPIValue *)"KiloBytes", CMPI_chars},
                {"VirtualQuantity", (CMPIValue *)&inc_vq, CMPI_uint64},
                PROP_END
        };

        ret = dup_rasd_prop_list(tmp, &rasd);
        if (!ret) {
                cu_statusf(_BROKER, s, 
                           CMPI_RC_ERR_FAILED,
                           "Could not copy RASD");
        }

        return rasd;
}

static struct sdc_rasd_prop *proc_min(const CMPIObjectPath *ref,
                                      CMPIStatus *s)
{
        bool ret;
        uint64_t num_procs = 1;
        struct sdc_rasd_prop *rasd = NULL;
 
        struct sdc_rasd_prop tmp[] = {
                {"InstanceID", (CMPIValue *)"Minimum", CMPI_chars},
                {"AllocationUnits", (CMPIValue *)"Processors", CMPI_chars},
                {"VirtualQuantity", (CMPIValue *)&num_procs, CMPI_uint64},
                PROP_END
        };
 
        ret = dup_rasd_prop_list(tmp, &rasd);
        if (!ret) {
                cu_statusf(_BROKER, s, 
                           CMPI_RC_ERR_FAILED,
                           "Could not copy RASD");
        }

        return rasd;
}

static struct sdc_rasd_prop *proc_max(const CMPIObjectPath *ref,
                                      CMPIStatus *s)
{
        bool ret;
        virConnectPtr conn;
        uint64_t num_procs = 0;
        struct sdc_rasd_prop *rasd = NULL;
        
        CU_DEBUG("In proc_max()");

        conn = connect_by_classname(_BROKER, CLASSNAME(ref), s);
        if (conn == NULL) {
                cu_statusf(_BROKER, s, 
                           CMPI_RC_ERR_FAILED,
                           "Could not connect to hypervisor");
                goto out;
        }

        num_procs = virConnectGetMaxVcpus(conn, NULL);
        CU_DEBUG("libvirt says %d max vcpus", num_procs);

        struct sdc_rasd_prop tmp[] = {
                {"InstanceID", (CMPIValue *)"Maximum", CMPI_chars},
                {"AllocationUnits", (CMPIValue *)"Processors", CMPI_chars},
                {"VirtualQuantity", (CMPIValue *)&num_procs, CMPI_uint64},
                PROP_END
        };
 
        ret = dup_rasd_prop_list(tmp, &rasd);
        if (!ret) {
                cu_statusf(_BROKER, s, 
                           CMPI_RC_ERR_FAILED,
                           "Could not copy RASD");
        }

 out:
        return rasd;
}

static struct sdc_rasd_prop *proc_def(const CMPIObjectPath *ref,
                                      CMPIStatus *s)
{
        bool ret;
        uint64_t num_procs = 1;
        struct sdc_rasd_prop *rasd = NULL;
 
        struct sdc_rasd_prop tmp[] = {
                {"InstanceID", (CMPIValue *)"Default", CMPI_chars},
                {"AllocationUnits", (CMPIValue *)"Processors", CMPI_chars},
                {"VirtualQuantity", (CMPIValue *)&num_procs, CMPI_uint64},
                PROP_END
        };
 
        ret = dup_rasd_prop_list(tmp, &rasd);
        if (!ret) {
                cu_statusf(_BROKER, s, 
                           CMPI_RC_ERR_FAILED,
                           "Could not copy RASD");
        }

        return rasd;
}

static struct sdc_rasd_prop *proc_inc(const CMPIObjectPath *ref,
                                      CMPIStatus *s)
{
        bool ret;
        uint64_t num_procs = 1;
        struct sdc_rasd_prop *rasd = NULL;
 
        struct sdc_rasd_prop tmp[] = {
                {"InstanceID", (CMPIValue *)"Increment", CMPI_chars},
                {"AllocationUnits", (CMPIValue *)"Processors", CMPI_chars},
                {"VirtualQuantity", (CMPIValue *)&num_procs, CMPI_uint64},
                PROP_END
        };
 
        ret = dup_rasd_prop_list(tmp, &rasd);
        if (!ret) {
                cu_statusf(_BROKER, s, 
                           CMPI_RC_ERR_FAILED,
                           "Could not copy RASD");
        }

        return rasd;
}

static struct sdc_rasd_prop *net_min(const CMPIObjectPath *ref,
                                     CMPIStatus *s)
{
        bool ret;
        uint64_t num_nics = 0;
        struct sdc_rasd_prop *rasd = NULL;
 
        struct sdc_rasd_prop tmp[] = {
                {"InstanceID", (CMPIValue *)"Minimum", CMPI_chars},
                {"VirtualQuantity", (CMPIValue *)&num_nics, CMPI_uint64},
                PROP_END
        };
 
        ret = dup_rasd_prop_list(tmp, &rasd);
        if (!ret) {
                cu_statusf(_BROKER, s, 
                           CMPI_RC_ERR_FAILED,
                           "Could not copy RASD");
        }

        return rasd;
}

static uint64_t net_max_kvm(const CMPIObjectPath *ref,
                            CMPIStatus *s)
{
        /* This appears to not require anything dynamic. */
        return KVM_MAX_NICS;
}
static uint64_t net_max_xen(const CMPIObjectPath *ref,
                            CMPIStatus *s)
{
        int rc;
        virConnectPtr conn;
        unsigned long version;
        uint64_t num_nics = -1;

        conn = connect_by_classname(_BROKER, CLASSNAME(ref), s);
        if (s->rc != CMPI_RC_OK) {
                cu_statusf(_BROKER, s, 
                           CMPI_RC_ERR_FAILED,
                           "Could not get connection");
                goto out;
        }

        rc = virConnectGetVersion(conn, &version);
        CU_DEBUG("libvir : version=%ld, rc=%d", version, rc);
        if (rc != 0) {
                cu_statusf(_BROKER, s, 
                           CMPI_RC_ERR_FAILED,
                           "Could not get xen version");
                goto out;
        }

        if (version >= 3001000)
                num_nics = XEN_MAX_NICS;
        else
                num_nics = 4;
        
 out:
        virConnectClose(conn);
        return num_nics;
}
 
static struct sdc_rasd_prop *net_max(const CMPIObjectPath *ref,
                                     CMPIStatus *s)
{
        bool ret;
        char *prefix;
        uint64_t num_nics;
        struct sdc_rasd_prop *rasd = NULL;

        prefix = class_prefix_name(CLASSNAME(ref));
        if (prefix == NULL) {
                cu_statusf(_BROKER, s,
                           CMPI_RC_ERR_FAILED,
                           "Could not get prefix from reference");
                goto out;
        }

        if (STREQC(prefix, "Xen")) {
                num_nics = net_max_xen(ref, s);
        } else if (STREQC(prefix, "KVM")) {
                num_nics = net_max_kvm(ref, s);
        } else {
                cu_statusf(_BROKER, s,
                           CMPI_RC_ERR_NOT_SUPPORTED,
                           "Unsupported hypervisor: '%s'", prefix);
                goto out;
        }
                

        if (s->rc != CMPI_RC_OK) {
                cu_statusf(_BROKER, s, 
                           CMPI_RC_ERR_FAILED,
                           "Could not get max nic count");
                goto out;
        }
 
        struct sdc_rasd_prop tmp[] = {
                {"InstanceID", (CMPIValue *)"Maximum", CMPI_chars},
                {"VirtualQuantity", (CMPIValue *)&num_nics, CMPI_uint64},
                PROP_END
        };
 
        ret = dup_rasd_prop_list(tmp, &rasd);
        if (!ret) {
                cu_statusf(_BROKER, s, 
                           CMPI_RC_ERR_FAILED,
                           "Could not copy RASD");
        }
 out:
        free(prefix);
        return rasd;
}

static struct sdc_rasd_prop *net_def(const CMPIObjectPath *ref,
                                     CMPIStatus *s)
{
        bool ret;
        uint64_t num_nics = 1;
        struct sdc_rasd_prop *rasd = NULL;
        struct sdc_rasd_prop tmp[] = {
                {"InstanceID", (CMPIValue *)"Default", CMPI_chars},
                {"VirtualQuantity", (CMPIValue *)&num_nics, CMPI_uint64},
                PROP_END
        };

        ret = dup_rasd_prop_list(tmp, &rasd);
        if (!ret) {
                cu_statusf(_BROKER, s, 
                           CMPI_RC_ERR_FAILED,
                           "Could not copy RASD");
        }

        return rasd;
}
 
static struct sdc_rasd_prop *net_inc(const CMPIObjectPath *ref,
                                     CMPIStatus *s)
{
        bool ret;
        uint64_t num_nics = 1;
        struct sdc_rasd_prop *rasd = NULL;
 
        struct sdc_rasd_prop tmp[] = {
                {"InstanceID", (CMPIValue *)"Increment", CMPI_chars},
                {"VirtualQuantity", (CMPIValue *)&num_nics, CMPI_uint64},
                PROP_END
        };
 
        ret = dup_rasd_prop_list(tmp, &rasd);
        if (!ret) {
                cu_statusf(_BROKER, s, 
                           CMPI_RC_ERR_FAILED,
                           "Could not copy RASD");
        }

        return rasd;
}

static struct sdc_rasd_prop *disk_min(const CMPIObjectPath *ref,
                                      CMPIStatus *s)
{
        bool ret;
        uint64_t disk_size = SDC_DISK_MIN;
        struct sdc_rasd_prop *rasd = NULL;

        struct sdc_rasd_prop tmp[] = {
                {"InstanceID", (CMPIValue *)"Minimum", CMPI_chars},
                {"AllocationQuantity", (CMPIValue *)"MegaBytes", CMPI_chars},
                {"VirtualQuantity", (CMPIValue *)&disk_size, CMPI_uint64},
                PROP_END
        };

        ret = dup_rasd_prop_list(tmp, &rasd);
        if (!ret) {
                cu_statusf(_BROKER, s, 
                           CMPI_RC_ERR_FAILED,
                           "Could not copy RASD");
        }

        return rasd;
}

static struct sdc_rasd_prop *disk_max(const CMPIObjectPath *ref,
                                      CMPIStatus *s)
{
        bool ret;
        const char *inst_id;
        CMPIrc prop_ret;
        uint64_t free_space;
        virConnectPtr conn;
        CMPIInstance *pool_inst;
        struct sdc_rasd_prop *rasd = NULL;
        
        if (cu_get_str_path(ref, "InstanceID", &inst_id) != CMPI_RC_OK) {
                cu_statusf(_BROKER, s, 
                           CMPI_RC_ERR_FAILED,
                           "Could not get InstanceID");
                goto out;
        }

        conn = connect_by_classname(_BROKER, CLASSNAME(ref), s);
        if (s->rc != CMPI_RC_OK) {
                cu_statusf(_BROKER, s, 
                           CMPI_RC_ERR_FAILED,
                           "Could not get connection");
                goto out;
        }

        /* Getting the relevant resource pool directly finds the free space 
           for us.  It is in the Capacity field. */
        *s = get_pool_by_name(_BROKER, ref, inst_id, &pool_inst);
        if (s->rc != CMPI_RC_OK)
                goto out;

        prop_ret = cu_get_u64_prop(pool_inst, "Capacity", &free_space);
        if (prop_ret != CMPI_RC_OK) {
                cu_statusf(_BROKER, s, 
                           CMPI_RC_ERR_FAILED,
                           "Could not get capacity from instance");
                goto out;
        }
        CU_DEBUG("Got capacity from pool_inst: %lld", free_space);

        struct sdc_rasd_prop tmp[] = {
                {"InstanceID", (CMPIValue *)"Maximum", CMPI_chars},
                {"AllocationQuantity", (CMPIValue *)"MegaBytes", CMPI_chars},
                {"VirtualQuantity", (CMPIValue *)&free_space, CMPI_uint64},
                PROP_END
        };

        ret = dup_rasd_prop_list(tmp, &rasd);
        if (!ret) {
                cu_statusf(_BROKER, s, 
                           CMPI_RC_ERR_FAILED,
                           "Could not copy RASD");
        }

 out:
        return rasd;
}

static struct sdc_rasd_prop *disk_def(const CMPIObjectPath *ref,
                                      CMPIStatus *s)
{
        bool ret;
        uint64_t disk_size = SDC_DISK_DEF;
        struct sdc_rasd_prop *rasd = NULL;

        struct sdc_rasd_prop tmp[] = {
                {"InstanceID", (CMPIValue *)"Default", CMPI_chars},
                {"AllocationQuantity", (CMPIValue *)"MegaBytes", CMPI_chars},
                {"VirtualQuantity", (CMPIValue *)&disk_size, CMPI_uint64},
                PROP_END
        };

        ret = dup_rasd_prop_list(tmp, &rasd);
        if (!ret) {
                cu_statusf(_BROKER, s, 
                           CMPI_RC_ERR_FAILED,
                           "Could not copy RASD");
        }

        return rasd;
}

static struct sdc_rasd_prop *disk_inc(const CMPIObjectPath *ref,
                                      CMPIStatus *s)
{
        bool ret;
        uint64_t disk_size = SDC_DISK_INC;
        struct sdc_rasd_prop *rasd = NULL;

        struct sdc_rasd_prop tmp[] = {
                {"InstanceID", (CMPIValue *)"Increment", CMPI_chars},
                {"AllocationQuantity", (CMPIValue *)"MegaBytes", CMPI_chars},
                {"VirtualQuantity", (CMPIValue *)&disk_size, CMPI_uint64},
                PROP_END
        };

        ret = dup_rasd_prop_list(tmp, &rasd);
        if (!ret) {
                cu_statusf(_BROKER, s, 
                           CMPI_RC_ERR_FAILED,
                           "Could not copy RASD");
        }

        return rasd;
}

static struct sdc_rasd mem = {
        .resource_type = CIM_RES_TYPE_MEM,
        .min = mem_min,
        .max = mem_max,
        .def = mem_def,
        .inc = mem_inc
};

static struct sdc_rasd processor = {
         .resource_type = CIM_RES_TYPE_PROC,
         .min = proc_min,
         .max = proc_max,
         .def = proc_def,
         .inc = proc_inc
};

static struct sdc_rasd network = {
        .resource_type = CIM_RES_TYPE_NET,
        .min = net_min,
        .max = net_max,
        .def = net_def,
        .inc = net_inc
};

static struct sdc_rasd disk = {
        .resource_type = CIM_RES_TYPE_DISK,
        .min = disk_min,
        .max = disk_max,
        .def = disk_def,
        .inc = disk_inc
};

static struct sdc_rasd *sdc_rasd_list[] = {
        &mem,
        &processor,
        &network,
        &disk,
        NULL
};

static CMPIInstance *sdc_rasd_inst(const CMPIBroker *broker,
                                   CMPIStatus *s,
                                   const CMPIObjectPath *ref,
                                   struct sdc_rasd *rasd,
                                   sdc_rasd_type type)
{
        CMPIInstance *inst = NULL;
        struct sdc_rasd_prop *prop_list = NULL;
        int i;
        const char *inst_id = NULL;
        const char *base = NULL;
        uint16_t resource_type;

        switch(type) {
        case SDC_RASD_MIN:
                if (rasd->min == NULL)
                        goto out;
                prop_list = rasd->min(ref, s);
                inst_id = "Minimum";
                break;
        case SDC_RASD_MAX:
                if (rasd->max == NULL)
                        goto out;
                prop_list = rasd->max(ref, s);
                inst_id = "Maximum";
                break;
        case SDC_RASD_INC:
                if (rasd->inc == NULL)
                        goto out;
                prop_list = rasd->inc(ref, s);
                inst_id = "Increment";
                break;
        case SDC_RASD_DEF:
                if (rasd->def == NULL)
                        goto out;
                prop_list = rasd->def(ref, s);
                inst_id = "Default";
                break;
        default:
                cu_statusf(broker, s, 
                           CMPI_RC_ERR_FAILED,
                           "Unsupported sdc_rasd type");
        }

        if (s->rc != CMPI_RC_OK) 
                goto out;

        if (rasd_classname_from_type(rasd->resource_type, &base) != CMPI_RC_OK) {
                cu_statusf(broker, s, 
                           CMPI_RC_ERR_FAILED,
                           "Resource type not known");
                goto out;
        }

        inst = get_typed_instance(broker,
                                  CLASSNAME(ref),
                                  base,
                                  NAMESPACE(ref));
        
        CMSetProperty(inst, "InstanceID", inst_id, CMPI_chars);

        resource_type = rasd->resource_type;
        CMSetProperty(inst, "ResourceType", &resource_type, CMPI_uint16);

        for (i = 0; prop_list[i].field != NULL; i++) {
                CU_DEBUG("Setting property '%s'", prop_list[i].field);
                CMSetProperty(inst, prop_list[i].field, 
                              prop_list[i].value, prop_list[i].type);
        }

 out:
        free_rasd_prop_list(prop_list);
        return inst;
}

static CMPIStatus sdc_rasds_for_type(const CMPIObjectPath *ref,
                                     struct inst_list *list,
                                     uint16_t type)
{
        CMPIStatus s = {CMPI_RC_OK, NULL};
        struct sdc_rasd *rasd = NULL;
        CMPIInstance *inst;
        int i;

        for (i = 0; sdc_rasd_list[i] != NULL; i++) {
                if (sdc_rasd_list[i]->resource_type == type) {
                        rasd = sdc_rasd_list[i];
                        break;
                }
        }

        if (rasd) {
                for (i = SDC_RASD_MIN; i <= SDC_RASD_INC; i++) {
                        inst = sdc_rasd_inst(_BROKER, &s, ref, rasd, i);
                        if (s.rc != CMPI_RC_OK) {
                                CU_DEBUG("Problem getting inst");
                                goto out;
                        }
                        CU_DEBUG("Got inst");
                        if (inst != NULL) {
                                inst_list_add(list, inst);
                                CU_DEBUG("Added inst");
                        } else {
                                CU_DEBUG("Inst is null, not added");
                        }
                }
                
        } else {
                CU_DEBUG("Unsupported type");
                cu_statusf(_BROKER, &s, 
                           CMPI_RC_ERR_FAILED,
                           "Unsupported device type");
        }

 out:
        return s;
}

static CMPIStatus alloc_cap_to_rasd(const CMPIObjectPath *ref,
                                    struct std_assoc_info *info,
                                    struct inst_list *list)
{
        CMPIStatus s = {CMPI_RC_OK};
        uint16_t type;
        const char *id = NULL;
        int i;

        if (!match_hypervisor_prefix(ref, info))
                return s;

        if (cu_get_str_path(ref, "InstanceID", &id) != CMPI_RC_OK) {
                cu_statusf(_BROKER, &s,
                           CMPI_RC_ERR_FAILED,
                           "Missing InstanceID");
                goto out;
        }
 
        type = res_type_from_pool_id(id);

        if (type == CIM_RES_TYPE_UNKNOWN) {
                cu_statusf(_BROKER, &s,
                           CMPI_RC_ERR_FAILED,
                           "Unable to determine resource type");
                goto out;
        }

        s = sdc_rasds_for_type(ref, list, type);

        for (i = 0; i < list->cur; i++)
                CMSetProperty(list->list[i], "PoolID",
                              (CMPIValue *)id, CMPI_chars);

 out:
        return s;
}

static CMPIStatus rasd_to_alloc_cap(const CMPIObjectPath *ref,
                                    struct std_assoc_info *info,
                                    struct inst_list *list)
{
        RETURN_UNSUPPORTED();
}

static CMPIStatus migrate_cap_to_vsmsd(const CMPIObjectPath *ref,
                                       struct std_assoc_info *info,
                                       struct inst_list *list)
{
        CMPIStatus s = {CMPI_RC_OK};
        CMPIInstance *inst;

        if (!match_hypervisor_prefix(ref, info))
                return s;

        s = get_migration_caps(ref, &inst, _BROKER, true);
        if (s.rc != CMPI_RC_OK)
                goto out;

        s = get_migration_sd(ref, &inst, _BROKER, false);
        if (s.rc == CMPI_RC_OK)
                inst_list_add(list, inst);

 out:
        return s;
}

static CMPIStatus vsmsd_to_migrate_cap(const CMPIObjectPath *ref,
                                       struct std_assoc_info *info,
                                       struct inst_list *list)
{
        CMPIStatus s = {CMPI_RC_OK};
        CMPIInstance *inst;

        if (!match_hypervisor_prefix(ref, info))
                return s;

        s = get_migration_sd(ref, &inst, _BROKER, true);
        if (s.rc != CMPI_RC_OK)
                goto out;

        s = get_migration_caps(ref, &inst, _BROKER, false);
        if (s.rc == CMPI_RC_OK)
                inst_list_add(list, inst);

 out:
        return s;
}

static CMPIInstance *make_ref_valuerole(const CMPIObjectPath *source_ref,
                                        const CMPIInstance *target_inst,
                                        struct std_assoc_info *info,
                                        struct std_assoc *assoc)
{
        CMPIInstance *ref_inst = NULL;
        uint16_t valuerole = SDC_ROLE_SUPPORTED;
        uint16_t valuerange;
        uint16_t ppolicy = SDC_POLICY_INDEPENDENT;
        const char *iid = NULL;

        ref_inst = make_reference(_BROKER,
                                  source_ref,
                                  target_inst,
                                  info,
                                  assoc);

        if (cu_get_str_prop(target_inst, "InstanceID", &iid) != CMPI_RC_OK) {
                CU_DEBUG("Target instance does not have an InstanceID");
                goto out;
        }

        if (strstr("Default", iid) != NULL)
                valuerange = SDC_RANGE_POINT;
        else if (strstr("Increment", iid) != NULL)
                valuerange = SDC_RANGE_INC;
        else if (strstr("Maximum", iid) != NULL)
                valuerange = SDC_RANGE_MAX;
        else if (strstr("Minimum", iid) != NULL)
                valuerange = SDC_RANGE_MIN;
        else
                CU_DEBUG("Unknown default RASD type: `%s'", iid);

        if (valuerange == SDC_RANGE_POINT)
                valuerole = SDC_ROLE_DEFAULT;

        CMSetProperty(ref_inst, "ValueRole",
                      (CMPIValue *)&valuerole, CMPI_uint16);
        CMSetProperty(ref_inst, "ValueRange",
                      (CMPIValue *)&valuerange, CMPI_uint16);
        CMSetProperty(ref_inst, "PropertyPolicy",
                      (CMPIValue *)&ppolicy, CMPI_uint16);
 out:
        return ref_inst;
}

LIBVIRT_CIM_DEFAULT_MAKEREF()

static char* group_component[] = {
        "Xen_AllocationCapabilities",
        "KVM_AllocationCapabilities",
        "LXC_AllocationCapabilities",
        NULL
};

static char* part_component[] = {
        "Xen_DiskResourceAllocationSettingData",
        "Xen_MemResourceAllocationSettingData",
        "Xen_NetResourceAllocationSettingData",
        "Xen_ProcResourceAllocationSettingData",
        "KVM_DiskResourceAllocationSettingData",
        "KVM_MemResourceAllocationSettingData",
        "KVM_NetResourceAllocationSettingData",
        "KVM_ProcResourceAllocationSettingData",
        "LXC_DiskResourceAllocationSettingData",
        "LXC_MemResourceAllocationSettingData",
        "LXC_NetResourceAllocationSettingData",
        "LXC_ProcResourceAllocationSettingData",
        NULL
};

static char* assoc_classname[] = {
        "Xen_SettingsDefineCapabilities",
        "KVM_SettingsDefineCapabilities",
        "LXC_SettingsDefineCapabilities",
        NULL
};

static struct std_assoc _alloc_cap_to_rasd = {
        .source_class = (char**)&group_component,
        .source_prop = "GroupComponent",

        .target_class = (char**)&part_component,
        .target_prop = "PartComponent",

        .assoc_class = (char**)&assoc_classname,

        .handler = alloc_cap_to_rasd,
        .make_ref = make_ref_valuerole
};

static struct std_assoc _rasd_to_alloc_cap = {
        .source_class = (char**)&part_component,
        .source_prop = "PartComponent",

        .target_class = (char**)&group_component,
        .target_prop = "GroupComponent",

        .assoc_class = (char**)&assoc_classname,

        .handler = rasd_to_alloc_cap,
        .make_ref = make_ref
};

static char* migrate_cap[] = {
        "Xen_VirtualSystemMigrationCapabilities",
        "KVM_VirtualSystemMigrationCapabilities",
        "LXC_VirtualSystemMigrationCapabilities",
        NULL
};

static char* migrate_sd[] = {
        "Xen_VirtualSystemMigrationSettingData",
        "KVM_VirtualSystemMigrationSettingData",
        "LXC_VirtualSystemMigrationSettingData",
        NULL
};

static struct std_assoc _migrate_cap_to_vsmsd = {
        .source_class = (char**)&migrate_cap,
        .source_prop = "GroupComponent",

        .target_class = (char**)&migrate_sd,
        .target_prop = "PartComponent",

        .assoc_class = (char**)&assoc_classname,

        .handler = migrate_cap_to_vsmsd,
        .make_ref = make_ref
};

static struct std_assoc _vsmsd_to_migrate_cap = {
        .source_class = (char**)&migrate_sd,
        .source_prop = "PartComponent",

        .target_class = (char**)&migrate_cap,
        .target_prop = "GroupComponent",

        .assoc_class = (char**)&assoc_classname,

        .handler = vsmsd_to_migrate_cap,
        .make_ref = make_ref
};

static char *vsmc[] = {
        "Xen_VirtualSystemManagementCapabilities",
        "KVM_VirtualSystemManagementCapabilities",
        "LXC_VirtualSystemManagementCapabilities",
        NULL
};

static char *vssd[] = {
        "Xen_VirtualSystemSettingData",
        "KVM_VirtualSystemSettingData",
        "LXC_VirtualSystemSettingData",
        NULL
};

static struct std_assoc _vsmc_to_vssd = {
        .source_class = (char**)&vsmc,
        .source_prop = "GroupComponent",

        .target_class = (char**)&vssd,
        .target_prop = "PartComponent",

        .assoc_class = (char**)&assoc_classname,

        .handler = vsmc_to_vssd,
        .make_ref = make_ref
};

static struct std_assoc *assoc_handlers[] = {
        &_alloc_cap_to_rasd,
        &_rasd_to_alloc_cap,
        &_migrate_cap_to_vsmsd,
        &_vsmsd_to_migrate_cap,
        &_vsmc_to_vssd,
        NULL
};

STDA_AssocMIStub(,
                 Virt_SettingsDefineCapabilities,
                 _BROKER, 
                 libvirt_cim_init(), 
                 assoc_handlers);

/*
 * Local Variables:
 * mode: C
 * c-set-style: "K&R"
 * tab-width: 8
 * c-basic-offset: 8
 * indent-tabs-mode: nil
 * End:
 */
