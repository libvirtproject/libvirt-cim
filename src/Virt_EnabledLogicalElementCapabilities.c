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
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <cmpidt.h>
#include <cmpift.h>
#include <cmpimacs.h>

#include <libcmpiutil/libcmpiutil.h>
#include <libcmpiutil/std_instance.h>

#include "misc_util.h"
#include "cs_util.h"

#include "Virt_EnabledLogicalElementCapabilities.h"

const static CMPIBroker *_BROKER;

enum {ENABLED = 2,
      DISABLED,
      SHUTDOWN,
      OFFLINE = 6,
      TEST,
      DEFER,
      QUIESCE,
      REBOOT,
      RESET};
      
                         

static CMPIStatus set_inst_properties(const CMPIBroker *broker,
                                      CMPIInstance *inst,
                                      const char *classname,
                                      const char *sys_name)
{
        CMPIStatus s = {CMPI_RC_OK, NULL};
        CMPIArray *array;
        uint16_t element;
        int edit_name = 0;
        
        CMSetProperty(inst, "CreationClassName",
                      (CMPIValue *)classname, CMPI_chars);

        CMSetProperty(inst, "InstanceID", (CMPIValue *)sys_name, CMPI_chars);

        array = CMNewArray(broker, 5, CMPI_uint16, &s);
        if ((s.rc != CMPI_RC_OK) || CMIsNullObject(array))
                goto out;
        
        element = (uint16_t)ENABLED;
        CMSetArrayElementAt(array, 0, &element, CMPI_uint16);

        element = (uint16_t)DISABLED;
        CMSetArrayElementAt(array, 1, &element, CMPI_uint16);

        element = (uint16_t)QUIESCE;
        CMSetArrayElementAt(array, 2, &element, CMPI_uint16);

        element = (uint16_t)REBOOT;
        CMSetArrayElementAt(array, 3, &element, CMPI_uint16);

        element = (uint16_t)RESET;
        CMSetArrayElementAt(array, 4, &element, CMPI_uint16);

        CMSetProperty(inst, "RequestedStatesSupported",
                      (CMPIValue *)&array, CMPI_uint16A);

        CMSetProperty(inst, "ElementNameEditSupported",
                      (CMPIValue *)&edit_name, CMPI_boolean);
 out:
        return s;
}

CMPIStatus get_ele_cap(const CMPIBroker *broker,
                       const CMPIObjectPath *ref,
                       const char *sys_name,
                       CMPIInstance **inst)
{
        CMPIStatus s;
        CMPIObjectPath *op;
        char *classname = NULL;

        classname = get_typed_class(CLASSNAME(ref),
                                    "EnabledLogicalElementCapabilities");
        if (classname == NULL) {
                cu_statusf(broker, &s,
                           CMPI_RC_ERR_FAILED,
                           "Invalid class");
                goto out;
        }

        op = CMNewObjectPath(broker, NAMESPACE(ref), classname, &s);
        if ((s.rc != CMPI_RC_OK) || CMIsNullObject(op)) {
                cu_statusf(broker, &s,
                           CMPI_RC_ERR_FAILED,
                           "Cannot get object path for ELECapabilities");
                goto out;
        }

        *inst = CMNewInstance(broker, op, &s);
        if ((s.rc != CMPI_RC_OK) || (CMIsNullObject(*inst))) {
                cu_statusf(broker, &s,
                           CMPI_RC_ERR_FAILED,
                           "Failed to instantiate HostSystem");
                goto out;
        }

        s = set_inst_properties(broker, *inst, classname, sys_name);

 out:
        free(classname);

        return s;
}

static CMPIStatus return_ele_cap(const CMPIObjectPath *ref,
                                 const CMPIResult *results,
                                 int names_only,
                                 const char *id)
{
        CMPIStatus s = {CMPI_RC_OK, NULL};
        CMPIInstance *inst = NULL;
        virConnectPtr conn = NULL;
        virDomainPtr *list = NULL;
        int count;
        int i;
        const char *name;

        conn = connect_by_classname(_BROKER, CLASSNAME(ref), &s);
        if (conn == NULL)
                goto out;

        count = get_domain_list(conn, &list);
        if (count <= 0)
                goto out;

        for (i = 0; i < count; i++) {
                name = virDomainGetName(list[i]);
                if (name == NULL) {
                        cu_statusf(_BROKER, &s,
                                   CMPI_RC_ERR_FAILED,
                                   "Unable to get domain names");
                        goto end;
                }

                if (id && (!STREQ(name, id)))
                        goto end;

                s = get_ele_cap(_BROKER, ref, name, &inst);
                if (s.rc != CMPI_RC_OK)
                        goto end;

                if (names_only)
                        cu_return_instance_name(results, inst);
                else
                        CMReturnInstance(results, inst);

          end:
                virDomainFree(list[i]);

                if ((s.rc != CMPI_RC_OK) || (id && (STREQ(name, id))))
                        goto out;
        }

 out:
        free(list);

        virConnectClose(conn);

        return s;
}

static CMPIStatus EnumInstanceNames(CMPIInstanceMI *self,
                                    const CMPIContext *context,
                                    const CMPIResult *results,
                                    const CMPIObjectPath *reference)
{
        return return_ele_cap(reference, results, 1, NULL);
}

static CMPIStatus EnumInstances(CMPIInstanceMI *self,
                                const CMPIContext *context,
                                const CMPIResult *results,
                                const CMPIObjectPath *reference,
                                const char **properties)
{

        return return_ele_cap(reference, results, 0, NULL);
}

static CMPIStatus GetInstance(CMPIInstanceMI *self,
                              const CMPIContext *context,
                              const CMPIResult *results,
                              const CMPIObjectPath *reference,
                              const char **properties)
{
        CMPIStatus s = {CMPI_RC_OK, NULL};
        const char* id;

        if (cu_get_str_path(reference, "InstanceID", &id) != CMPI_RC_OK) {
                cu_statusf(_BROKER, &s,
                           CMPI_RC_ERR_FAILED,
                           "No InstanceID specified");
                return s;
        }

        return return_ele_cap(reference, results, 0, id);
}

DEFAULT_CI();
DEFAULT_MI();
DEFAULT_DI();
DEFAULT_EQ();
DEFAULT_INST_CLEANUP();

STD_InstanceMIStub(, Virt_EnabledLogicalElementCapabilitiesProvider, _BROKER,
                 libvirt_cim_init());

/*
 * Local Variables:
 * mode: C
 * c-set-style: "K&R"
 * tab-width: 8
 * c-basic-offset: 8
 * indent-tabs-mode: nil
 * End:
 */
