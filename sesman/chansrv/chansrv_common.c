/**
 * xrdp: A Remote Desktop Protocol server.
 *
 * Copyright (C) Laxmikant Rashinkar 2009-2014
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#if defined(HAVE_CONFIG_H)
#include <config_ac.h>
#endif

#include "chansrv_common.h"

/**
 * Assemble fragmented incoming packets into one stream
 *
 * @param  src           stream that contains partial data
 * @param  dest          stream that contains entire data
 * @param  chan_flags    fragmentation flags
 * @param  length        bytes in this packet
 * @param  total_length  total length of assembled packet
 *
 * @return 1 when all data has been assembled, 0 otherwise
 *
 * NOTE: it is the responsibility of the caller to free dest stream
 ****************************************************************************/
int
read_entire_packet(struct stream *src, struct stream **dest, int chan_flags,
                   int length, int total_length)
{
    struct stream *ls;

    if ((chan_flags & 3) == 3)
    {
        /* packet not fragmented */
        xstream_new(ls, total_length);
        if (ls->data != NULL)
        {
            xstream_copyin(ls, src->p, length);
            s_mark_end(ls);
            ls->p = ls->data;
            *dest = ls;
            return 1;
        }
        else
        {
            LOG(LOG_LEVEL_ERROR, "read_entire_packet: failed to allocate memory for stream");
            xstream_free(ls);
            return 0;
        }
    }

    /* is this the first fragmented packet? */
    if (chan_flags & 1)
    {
        xstream_new(ls, total_length);
        if (ls->data == NULL)
        {
            LOG(LOG_LEVEL_ERROR, "read_entire_packet: failed to allocate memory for stream");
            xstream_free(ls);
            return 0;
        }
        *dest = ls;
    }
    else
    {
        ls = *dest;
    }

    xstream_copyin(ls, src->p, length);

    /* in last packet, chan_flags & 0x02 will be true */
    if (chan_flags & 0x02)
    {
        /* terminate and rewind stream */
        s_mark_end(ls);
        if (ls->data != NULL)
        {
            ls->p = ls->data;
        }
        else
        {
            LOG(LOG_LEVEL_ERROR, "read_entire_packet: stream data is NULL");
        }
        return 1;
    }

    return 0;
}
