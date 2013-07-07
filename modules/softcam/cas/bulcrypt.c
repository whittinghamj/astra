/*
 * Astra SoftCAM module
 * Copyright (C) 2012-2013, Andrey Dyldin <and@cesbo.com>
 *               2012, Georgi Chorbadzhiyski <gf@unixsol.org>
 *
 * This module is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This module is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this module.  If not, see <http://www.gnu.org/licenses/>.
 *
 * For more information, visit http://cesbo.com
 */

#include "../module_cam.h"

struct module_data_t
{
    MODULE_CAS_DATA();

    uint8_t parity;
};

static bool cas_check_em(module_data_t *mod, mpegts_psi_t *em)
{
    const uint8_t em_type = em->buffer[0];
    switch(em_type)
    {
        // ECM
        case 0x80:
        case 0x81:
        {
            if(em_type != mod->parity)
            {
                mod->parity = em_type;
                return true;
            }
            break;
        }
        case 0x82:
        case 0x85:
        { // unique
            if(!memcmp(&em->buffer[3], &mod->__cas.decrypt->cam->ua[2], 3))
                return true;
            break;
        }
        case 0x84:
        { // shared
            if(!memcmp(&em->buffer[3], &mod->__cas.decrypt->cam->ua[2], 2))
                return true;
            break;
        }
        case 0x8b:
        { //shared-unknown
            if(!memcmp(&em->buffer[4], &mod->__cas.decrypt->cam->ua[2], 2))
                return true;
            break;
        }
        case 0x8a:
        { // global
            if(em->buffer[4] == mod->__cas.decrypt->cam->ua[2])
                return true;
            break;
        }
        default:
            break;
    }

    return false;
}

static bool cas_check_keys(module_data_t *mod, const uint8_t *keys)
{
    __uarg(mod);
    __uarg(keys);
    return true;
}

/*
 * CA descriptor (iso13818-1):
 * tag      :8 (must be 0x09)
 * length   :8
 * caid     :16
 * reserved :3
 * pid      :13
 * data     :length-4
 */

static bool cas_check_descriptor(module_data_t *mod, const uint8_t *desc)
{
    __uarg(mod);
    __uarg(desc);
    return true;
}

static bool cas_check_caid(uint16_t caid)
{
    return (caid == 0x4AEE || caid == 0x5581);
}

MODULE_CAS(bulcrypt)