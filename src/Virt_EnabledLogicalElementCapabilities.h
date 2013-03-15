/*
 * Copyright IBM Corp. 2007
 *
 * Authors:
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
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */
CMPIStatus get_elec_by_name(const CMPIBroker *broker,
                            const CMPIObjectPath *reference,
                            const char *name,
                            CMPIInstance **_inst);

CMPIStatus get_elec_by_ref(const CMPIBroker *broker,
                           const CMPIObjectPath *reference,
                           CMPIInstance **_inst);

/*
 * Local Variables:
 * mode: C
 * c-set-style: "K&R"
 * tab-width: 8
 * c-basic-offset: 8
 * indent-tabs-mode: nil
 * End:
 */
