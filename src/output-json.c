/* Copyright (C) 2007-2018 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Tom DeCanio <td@npulsetech.com>
 *
 * Logs detection and monitoring events in JSON format.
 *
 */

#include "suricata-common.h"
#include "debug.h"
#include "detect.h"
#include "flow.h"
#include "conf.h"

#include "threads.h"
#include "tm-threads.h"
#include "threadvars.h"
#include "util-debug.h"

#include "util-unittest.h"
#include "util-unittest-helper.h"

#include "detect-parse.h"
#include "detect-engine.h"
#include "detect-engine-mpm.h"
#include "detect-reference.h"
#include "app-layer-parser.h"
#include "util-classification-config.h"
#include "util-syslog.h"

#include "output.h"
#include "output-json.h"

#include "util-byte.h"
#include "util-privs.h"
#include "util-print.h"
#include "util-proto-name.h"
#include "util-optimize.h"
#include "util-buffer.h"
#include "util-logopenfile.h"
#include "util-log-redis.h"
#include "util-device.h"
#include "util-validate.h"
#include "util-crypt.h"

#include "flow-var.h"
#include "flow-bit.h"

#include "source-pcap-file.h"

#define DEFAULT_LOG_FILENAME "eve.json"
#define DEFAULT_ALERT_SYSLOG_FACILITY_STR       "local0"
#define DEFAULT_ALERT_SYSLOG_FACILITY           LOG_LOCAL0
#define DEFAULT_ALERT_SYSLOG_LEVEL              LOG_INFO
#define MODULE_NAME "OutputJSON"

#define MAX_JSON_SIZE 2048

static void OutputJsonDeInitCtx(OutputCtx *);
static void CreateJSONCommunityFlowId(json_t *js, const Flow *f, const uint16_t seed);
static void CreateEveCommunityFlowId(JsonBuilder *js, const Flow *f, const uint16_t seed);

static const char *TRAFFIC_ID_PREFIX = "traffic/id/";
static const char *TRAFFIC_LABEL_PREFIX = "traffic/label/";
static size_t traffic_id_prefix_len = 0;
static size_t traffic_label_prefix_len = 0;

const JsonAddrInfo json_addr_info_zero;

void OutputJsonRegister (void)
{
    OutputRegisterModule(MODULE_NAME, "eve-log", OutputJsonInitCtx);

    traffic_id_prefix_len = strlen(TRAFFIC_ID_PREFIX);
    traffic_label_prefix_len = strlen(TRAFFIC_LABEL_PREFIX);
}

json_t *SCJsonBool(int val)
{
    return (val ? json_true() : json_false());
}

/**
 * Wrap json_decref. This is mainly to expose this function to Rust as its
 * defined in the Jansson header file as an inline function.
 */
void SCJsonDecref(json_t *json)
{
    json_decref(json);
}

json_t *SCJsonString(const char *val)
{
    if (val == NULL){
        return NULL;
    }
    json_t * retval = json_string(val);
    char retbuf[MAX_JSON_SIZE] = {0};
    if (retval == NULL) {
        uint32_t u = 0;
        uint32_t offset = 0;
        for (u = 0; u < strlen(val); u++) {
            if (isprint(val[u])) {
                PrintBufferData(retbuf, &offset, MAX_JSON_SIZE-1, "%c",
                        val[u]);
            } else {
                PrintBufferData(retbuf, &offset, MAX_JSON_SIZE-1,
                        "\\x%02X", val[u]);
            }
        }
        retbuf[offset] = '\0';
        retval = json_string(retbuf);
    }
    return retval;
}

/* Default Sensor ID value */
static int64_t sensor_id = -1; /* -1 = not defined */

/**
 * \brief Create a JSON string from a character sequence
 *
 * \param Pointer to character sequence
 * \param Number of characters to use from the sequence
 * \retval JSON object for the character sequence
 */
json_t *JsonAddStringN(const char *string, size_t size)
{
    char tmpbuf[size + 1];

    memcpy(tmpbuf, string, size);
    tmpbuf[size] = '\0';

    return SCJsonString(tmpbuf);
}

static void JsonAddPacketvars(const Packet *p, json_t *js_vars)
{
    if (p == NULL || p->pktvar == NULL) {
        return;
    }
    json_t *js_pktvars = NULL;
    PktVar *pv = p->pktvar;
    while (pv != NULL) {
        if (pv->key || pv->id > 0) {
            if (js_pktvars == NULL) {
                js_pktvars = json_array();
                if (js_pktvars == NULL)
                    break;
            }
            json_t *js_pair = json_object();
            if (js_pair == NULL) {
                break;
            }

            if (pv->key != NULL) {
                uint32_t offset = 0;
                uint8_t keybuf[pv->key_len + 1];
                PrintStringsToBuffer(keybuf, &offset,
                        sizeof(keybuf),
                        pv->key, pv->key_len);
                uint32_t len = pv->value_len;
                uint8_t printable_buf[len + 1];
                offset = 0;
                PrintStringsToBuffer(printable_buf, &offset,
                        sizeof(printable_buf),
                        pv->value, pv->value_len);
                json_object_set_new(js_pair, (char *)keybuf,
                        json_string((char *)printable_buf));
            } else {
                const char *varname = VarNameStoreLookupById(pv->id, VAR_TYPE_PKT_VAR);
                uint32_t len = pv->value_len;
                uint8_t printable_buf[len + 1];
                uint32_t offset = 0;
                PrintStringsToBuffer(printable_buf, &offset,
                        sizeof(printable_buf),
                        pv->value, pv->value_len);

                json_object_set_new(js_pair, varname,
                        json_string((char *)printable_buf));
            }
            json_array_append_new(js_pktvars, js_pair);
        }
        pv = pv->next;
    }
    if (js_pktvars) {
        json_object_set_new(js_vars, "pktvars", js_pktvars);
    }
}

static void EveAddPacketVars(const Packet *p, JsonBuilder *js_vars)
{
    if (p == NULL || p->pktvar == NULL) {
        return;
    }
    PktVar *pv = p->pktvar;
    bool open = false;
    while (pv != NULL) {
        if (pv->key || pv->id > 0) {
            if (!open) {
                jb_open_array(js_vars, "pktvars");
                open = true;
            }
            jb_start_object(js_vars);

            if (pv->key != NULL) {
                uint32_t offset = 0;
                uint8_t keybuf[pv->key_len + 1];
                PrintStringsToBuffer(keybuf, &offset,
                        sizeof(keybuf),
                        pv->key, pv->key_len);
                uint32_t len = pv->value_len;
                uint8_t printable_buf[len + 1];
                offset = 0;
                PrintStringsToBuffer(printable_buf, &offset,
                        sizeof(printable_buf),
                        pv->value, pv->value_len);
                jb_set_string(js_vars, (char *)keybuf, (char *)printable_buf);
            } else {
                const char *varname = VarNameStoreLookupById(pv->id, VAR_TYPE_PKT_VAR);
                uint32_t len = pv->value_len;
                uint8_t printable_buf[len + 1];
                uint32_t offset = 0;
                PrintStringsToBuffer(printable_buf, &offset,
                        sizeof(printable_buf),
                        pv->value, pv->value_len);
                jb_set_string(js_vars, varname, (char *)printable_buf);
            }
            jb_close(js_vars);
        }
        pv = pv->next;
    }
    if (open) {
        jb_close(js_vars);
    }
}

/**
 * \brief Check if string s has prefix prefix.
 *
 * \retval true if string has prefix
 * \retval false if string does not have prefix
 *
 * TODO: Move to file with other string handling functions.
 */
static bool SCStringHasPrefix(const char *s, const char *prefix)
{
    if (strncmp(s, prefix, strlen(prefix)) == 0) {
        return true;
    }
    return false;
}

/**
 * \brief Add flow variables to a json object.
 *
 * Adds "flowvars" (map), "flowints" (map) and "flowbits" (array) to
 * the json object provided as js_root.
 */
static void JsonAddFlowVars(const Flow *f, json_t *js_root, json_t **js_traffic)
{
    if (f == NULL || f->flowvar == NULL) {
        return;
    }
    json_t *js_flowvars = NULL;
    json_t *js_traffic_id = NULL;
    json_t *js_traffic_label = NULL;
    json_t *js_flowints = NULL;
    json_t *js_flowbits = NULL;
    GenericVar *gv = f->flowvar;
    while (gv != NULL) {
        if (gv->type == DETECT_FLOWVAR || gv->type == DETECT_FLOWINT) {
            FlowVar *fv = (FlowVar *)gv;
            if (fv->datatype == FLOWVAR_TYPE_STR && fv->key == NULL) {
                const char *varname = VarNameStoreLookupById(fv->idx,
                        VAR_TYPE_FLOW_VAR);
                if (varname) {
                    if (js_flowvars == NULL) {
                        js_flowvars = json_array();
                        if (js_flowvars == NULL)
                            break;
                    }

                    uint32_t len = fv->data.fv_str.value_len;
                    uint8_t printable_buf[len + 1];
                    uint32_t offset = 0;
                    PrintStringsToBuffer(printable_buf, &offset,
                            sizeof(printable_buf),
                            fv->data.fv_str.value, fv->data.fv_str.value_len);

                    json_t *js_flowvar = json_object();
                    if (unlikely(js_flowvar == NULL)) {
                        break;
                    }
                    json_object_set_new(js_flowvar, varname,
                            json_string((char *)printable_buf));
                    json_array_append_new(js_flowvars, js_flowvar);
                }
            } else if (fv->datatype == FLOWVAR_TYPE_STR && fv->key != NULL) {
                if (js_flowvars == NULL) {
                    js_flowvars = json_array();
                    if (js_flowvars == NULL)
                        break;
                }

                uint8_t keybuf[fv->keylen + 1];
                uint32_t offset = 0;
                PrintStringsToBuffer(keybuf, &offset,
                        sizeof(keybuf),
                        fv->key, fv->keylen);

                uint32_t len = fv->data.fv_str.value_len;
                uint8_t printable_buf[len + 1];
                offset = 0;
                PrintStringsToBuffer(printable_buf, &offset,
                        sizeof(printable_buf),
                        fv->data.fv_str.value, fv->data.fv_str.value_len);

                json_t *js_flowvar = json_object();
                if (unlikely(js_flowvar == NULL)) {
                    break;
                }
                json_object_set_new(js_flowvar, (const char *)keybuf,
                        json_string((char *)printable_buf));
                json_array_append_new(js_flowvars, js_flowvar);
            } else if (fv->datatype == FLOWVAR_TYPE_INT) {
                const char *varname = VarNameStoreLookupById(fv->idx,
                        VAR_TYPE_FLOW_INT);
                if (varname) {
                    if (js_flowints == NULL) {
                        js_flowints = json_object();
                        if (js_flowints == NULL)
                            break;
                    }

                    json_object_set_new(js_flowints, varname,
                            json_integer(fv->data.fv_int.value));
                }

            }
        } else if (gv->type == DETECT_FLOWBITS) {
            FlowBit *fb = (FlowBit *)gv;
            const char *varname = VarNameStoreLookupById(fb->idx,
                    VAR_TYPE_FLOW_BIT);
            if (varname) {
                if (SCStringHasPrefix(varname, TRAFFIC_ID_PREFIX)) {
                    if (js_traffic_id == NULL) {
                        js_traffic_id = json_array();
                        if (unlikely(js_traffic_id == NULL)) {
                            break;
                        }
                    }
                    json_array_append_new(js_traffic_id,
                            json_string(&varname[traffic_id_prefix_len]));
                } else if (SCStringHasPrefix(varname, TRAFFIC_LABEL_PREFIX)) {
                    if (js_traffic_label == NULL) {
                        js_traffic_label = json_array();
                        if (unlikely(js_traffic_label == NULL)) {
                            break;
                        }
                    }
                    json_array_append_new(js_traffic_label,
                            json_string(&varname[traffic_label_prefix_len]));
                } else {
                    if (js_flowbits == NULL) {
                        js_flowbits = json_array();
                        if (unlikely(js_flowbits == NULL))
                            break;
                    }
                    json_array_append_new(js_flowbits, json_string(varname));
                }
            }
        }
        gv = gv->next;
    }
    if (js_flowbits) {
        json_object_set_new(js_root, "flowbits", js_flowbits);
    }
    if (js_flowints) {
        json_object_set_new(js_root, "flowints", js_flowints);
    }
    if (js_flowvars) {
        json_object_set_new(js_root, "flowvars", js_flowvars);
    }

    if (js_traffic_id != NULL || js_traffic_label != NULL) {
        *js_traffic = json_object();
        if (likely(*js_traffic != NULL)) {
            if (js_traffic_id != NULL) {
                json_object_set_new(*js_traffic, "id", js_traffic_id);
            }
            if (js_traffic_label != NULL) {
                json_object_set_new(*js_traffic, "label", js_traffic_label);
            }
        }
    }
}

static void EveAddFlowVars(const Flow *f, JsonBuilder *js_root, JsonBuilder **js_traffic)
{
    if (f == NULL || f->flowvar == NULL) {
        return;
    }
    JsonBuilder *js_flowvars = NULL;
    JsonBuilder *js_traffic_id = NULL;
    JsonBuilder *js_traffic_label = NULL;
    JsonBuilder *js_flowints = NULL;
    JsonBuilder *js_flowbits = NULL;
    GenericVar *gv = f->flowvar;
    while (gv != NULL) {
        if (gv->type == DETECT_FLOWVAR || gv->type == DETECT_FLOWINT) {
            FlowVar *fv = (FlowVar *)gv;
            if (fv->datatype == FLOWVAR_TYPE_STR && fv->key == NULL) {
                const char *varname = VarNameStoreLookupById(fv->idx,
                        VAR_TYPE_FLOW_VAR);
                if (varname) {
                    if (js_flowvars == NULL) {
                        js_flowvars = jb_new_array();
                        if (js_flowvars == NULL)
                            break;
                    }

                    uint32_t len = fv->data.fv_str.value_len;
                    uint8_t printable_buf[len + 1];
                    uint32_t offset = 0;
                    PrintStringsToBuffer(printable_buf, &offset,
                            sizeof(printable_buf),
                            fv->data.fv_str.value, fv->data.fv_str.value_len);

                    jb_start_object(js_flowvars);
                    jb_set_string(js_flowvars, varname, (char *)printable_buf);
                    jb_close(js_flowvars);
                }
            } else if (fv->datatype == FLOWVAR_TYPE_STR && fv->key != NULL) {
                if (js_flowvars == NULL) {
                    js_flowvars = jb_new_array();
                    if (js_flowvars == NULL)
                        break;
                }

                uint8_t keybuf[fv->keylen + 1];
                uint32_t offset = 0;
                PrintStringsToBuffer(keybuf, &offset,
                        sizeof(keybuf),
                        fv->key, fv->keylen);

                uint32_t len = fv->data.fv_str.value_len;
                uint8_t printable_buf[len + 1];
                offset = 0;
                PrintStringsToBuffer(printable_buf, &offset,
                        sizeof(printable_buf),
                        fv->data.fv_str.value, fv->data.fv_str.value_len);

                jb_start_object(js_flowvars);
                jb_set_string(js_flowvars, (const char *)keybuf, (char *)printable_buf);
                jb_close(js_flowvars);
            } else if (fv->datatype == FLOWVAR_TYPE_INT) {
                const char *varname = VarNameStoreLookupById(fv->idx,
                        VAR_TYPE_FLOW_INT);
                if (varname) {
                    if (js_flowints == NULL) {
                        js_flowints = jb_new_object();
                        if (js_flowints == NULL)
                            break;
                    }
                    jb_set_uint(js_flowints, varname, fv->data.fv_int.value);
                }

            }
        } else if (gv->type == DETECT_FLOWBITS) {
            FlowBit *fb = (FlowBit *)gv;
            const char *varname = VarNameStoreLookupById(fb->idx,
                    VAR_TYPE_FLOW_BIT);
            if (varname) {
                if (SCStringHasPrefix(varname, TRAFFIC_ID_PREFIX)) {
                    if (js_traffic_id == NULL) {
                        js_traffic_id = jb_new_array();
                        if (unlikely(js_traffic_id == NULL)) {
                            break;
                        }
                    }
                    jb_append_string(js_traffic_id, &varname[traffic_id_prefix_len]);
                } else if (SCStringHasPrefix(varname, TRAFFIC_LABEL_PREFIX)) {
                    if (js_traffic_label == NULL) {
                        js_traffic_label = jb_new_array();
                        if (unlikely(js_traffic_label == NULL)) {
                            break;
                        }
                    }
                    jb_append_string(js_traffic_label, &varname[traffic_label_prefix_len]);
                } else {
                    if (js_flowbits == NULL) {
                        js_flowbits = jb_new_array();
                        if (unlikely(js_flowbits == NULL))
                            break;
                    }
                    jb_append_string(js_flowbits, varname);
                }
            }
        }
        gv = gv->next;
    }
    if (js_flowbits) {
        jb_close(js_flowbits);
        jb_set_object(js_root, "flowbits", js_flowbits);
        jb_free(js_flowbits);
    }
    if (js_flowints) {
        jb_close(js_flowints);
        jb_set_object(js_root, "flowints", js_flowints);
        jb_free(js_flowints);
    }
    if (js_flowvars) {
        jb_close(js_flowvars);
        jb_set_object(js_root, "flowvars", js_flowvars);
        jb_free(js_flowvars);
    }

    if (js_traffic_id != NULL || js_traffic_label != NULL) {
        *js_traffic = jb_new_object();
        if (likely(*js_traffic != NULL)) {
            if (js_traffic_id != NULL) {
                jb_close(js_traffic_id);
                jb_set_object(*js_traffic, "id", js_traffic_id);
                jb_free(js_traffic_id);
            }
            if (js_traffic_label != NULL) {
                jb_close(js_traffic_label);
                jb_set_object(*js_traffic, "label", js_traffic_label);
                jb_free(js_traffic_label);
            }
            jb_close(*js_traffic);
        }
    }
}

/**
 * \brief Add top-level metadata to the eve json object.
 */
static void JsonAddMetadata(const Packet *p, const Flow *f, json_t *js)
{
    if ((p && p->pktvar) || (f && f->flowvar)) {
        json_t *js_vars = json_object();
        json_t *js_traffic = NULL;
        if (js_vars) {
            if (f && f->flowvar) {
                JsonAddFlowVars(f, js_vars, &js_traffic);
                if (js_traffic != NULL) {
                    json_object_set_new(js, "traffic", js_traffic);
                }
            }
            if (p && p->pktvar) {
                JsonAddPacketvars(p, js_vars);
            }

            json_object_set_new(js, "metadata", js_vars);
        }
    }
}

static void EveAddMetadata(const Packet *p, const Flow *f, JsonBuilder *js)
{
    if ((p && p->pktvar) || (f && f->flowvar)) {
        JsonBuilder *js_vars = jb_new_object();
        if (js_vars) {
            if (f && f->flowvar) {
                JsonBuilder *js_traffic = NULL;
                EveAddFlowVars(f, js_vars, &js_traffic);
                if (js_traffic != NULL) {
                    jb_set_object(js, "traffic", js_traffic);
                    jb_free(js_traffic);
                }
            }
            if (p && p->pktvar) {
                EveAddPacketVars(p, js_vars);
            }
            jb_close(js_vars);
            jb_set_object(js, "metadata", js_vars);
            jb_free(js_vars);
        }
    }
}

void JsonAddCommonOptions(const OutputJsonCommonSettings *cfg,
        const Packet *p, const Flow *f, json_t *js)
{
    if (cfg->include_metadata) {
        JsonAddMetadata(p, f, js);
    }
    if (cfg->include_community_id && f != NULL) {
        CreateJSONCommunityFlowId(js, f, cfg->community_id_seed);
    }
}

void EveAddCommonOptions(const OutputJsonCommonSettings *cfg,
        const Packet *p, const Flow *f, JsonBuilder *js)
{
    if (cfg->include_metadata) {
        EveAddMetadata(p, f, js);
    }
    if (cfg->include_community_id && f != NULL) {
        CreateEveCommunityFlowId(js, f, cfg->community_id_seed);
    }
}

/**
 * \brief Jsonify a packet
 *
 * \param p Packet
 * \param js JSON object
 * \param max_length If non-zero, restricts the number of packet data bytes handled.
 */
void JsonPacket(const Packet *p, json_t *js, unsigned long max_length)
{
    unsigned long max_len = max_length == 0 ? GET_PKT_LEN(p) : max_length;
    unsigned long len = 2 * max_len;
    uint8_t encoded_packet[len];
    if (Base64Encode((unsigned char*) GET_PKT_DATA(p), max_len, encoded_packet, &len) == SC_BASE64_OK) {
        json_object_set_new(js, "packet", json_string((char *)encoded_packet));
    }

    /* Create packet info. */
    json_t *packetinfo_js = json_object();
    if (unlikely(packetinfo_js == NULL)) {
        return;
    }
    json_object_set_new(packetinfo_js, "linktype", json_integer(p->datalink));
    json_object_set_new(js, "packet_info", packetinfo_js);
}

/**
 * \brief Jsonify a packet
 *
 * \param p Packet
 * \param js JSON object
 * \param max_length If non-zero, restricts the number of packet data bytes handled.
 */
void EvePacket(const Packet *p, JsonBuilder *js, unsigned long max_length)
{
    unsigned long max_len = max_length == 0 ? GET_PKT_LEN(p) : max_length;
    unsigned long len = 2 * max_len;
    uint8_t encoded_packet[len];
    if (Base64Encode((unsigned char*) GET_PKT_DATA(p), max_len, encoded_packet, &len) == SC_BASE64_OK) {
        jb_set_string(js, "packet", (char *)encoded_packet);
    }

    if (!jb_open_object(js, "packet_info")) {
        return;
    }
    if (!jb_set_uint(js, "linktype", p->datalink)) {
        return;
    }
    jb_close(js);
}

/** \brief jsonify tcp flags field
 *  Only add 'true' fields in an attempt to keep things reasonably compact.
 */
void JsonTcpFlags(uint8_t flags, json_t *js)
{
    if (flags & TH_SYN)
        json_object_set_new(js, "syn", json_true());
    if (flags & TH_FIN)
        json_object_set_new(js, "fin", json_true());
    if (flags & TH_RST)
        json_object_set_new(js, "rst", json_true());
    if (flags & TH_PUSH)
        json_object_set_new(js, "psh", json_true());
    if (flags & TH_ACK)
        json_object_set_new(js, "ack", json_true());
    if (flags & TH_URG)
        json_object_set_new(js, "urg", json_true());
    if (flags & TH_ECN)
        json_object_set_new(js, "ecn", json_true());
    if (flags & TH_CWR)
        json_object_set_new(js, "cwr", json_true());
}

/** \brief jsonify tcp flags field
 *  Only add 'true' fields in an attempt to keep things reasonably compact.
 */
void EveTcpFlags(const uint8_t flags, JsonBuilder *js)
{
    if (flags & TH_SYN)
        jb_set_bool(js, "syn", true);
    if (flags & TH_FIN)
        jb_set_bool(js, "fin", true);
    if (flags & TH_RST)
        jb_set_bool(js, "rst", true);
    if (flags & TH_PUSH)
        jb_set_bool(js, "psh", true);
    if (flags & TH_ACK)
        jb_set_bool(js, "ack", true);
    if (flags & TH_URG)
        jb_set_bool(js, "urg", true);
    if (flags & TH_ECN)
        jb_set_bool(js, "ecn", true);
    if (flags & TH_CWR)
        jb_set_bool(js, "cwr", true);
}

void JsonAddrInfoInit(const Packet *p, enum OutputJsonLogDirection dir, JsonAddrInfo *addr)
{
    char srcip[46] = {0}, dstip[46] = {0};
    Port sp, dp;
    char proto[16];

    switch (dir) {
        case LOG_DIR_PACKET:
            if (PKT_IS_IPV4(p)) {
                PrintInet(AF_INET, (const void *)GET_IPV4_SRC_ADDR_PTR(p),
                        srcip, sizeof(srcip));
                PrintInet(AF_INET, (const void *)GET_IPV4_DST_ADDR_PTR(p),
                        dstip, sizeof(dstip));
            } else if (PKT_IS_IPV6(p)) {
                PrintInet(AF_INET6, (const void *)GET_IPV6_SRC_ADDR(p),
                        srcip, sizeof(srcip));
                PrintInet(AF_INET6, (const void *)GET_IPV6_DST_ADDR(p),
                        dstip, sizeof(dstip));
            } else {
                /* Not an IP packet so don't do anything */
                return;
            }
            sp = p->sp;
            dp = p->dp;
            break;
        case LOG_DIR_FLOW:
        case LOG_DIR_FLOW_TOSERVER:
            if ((PKT_IS_TOSERVER(p))) {
                if (PKT_IS_IPV4(p)) {
                    PrintInet(AF_INET, (const void *)GET_IPV4_SRC_ADDR_PTR(p),
                            srcip, sizeof(srcip));
                    PrintInet(AF_INET, (const void *)GET_IPV4_DST_ADDR_PTR(p),
                            dstip, sizeof(dstip));
                } else if (PKT_IS_IPV6(p)) {
                    PrintInet(AF_INET6, (const void *)GET_IPV6_SRC_ADDR(p),
                            srcip, sizeof(srcip));
                    PrintInet(AF_INET6, (const void *)GET_IPV6_DST_ADDR(p),
                            dstip, sizeof(dstip));
                }
                sp = p->sp;
                dp = p->dp;
            } else {
                if (PKT_IS_IPV4(p)) {
                    PrintInet(AF_INET, (const void *)GET_IPV4_DST_ADDR_PTR(p),
                            srcip, sizeof(srcip));
                    PrintInet(AF_INET, (const void *)GET_IPV4_SRC_ADDR_PTR(p),
                            dstip, sizeof(dstip));
                } else if (PKT_IS_IPV6(p)) {
                    PrintInet(AF_INET6, (const void *)GET_IPV6_DST_ADDR(p),
                            srcip, sizeof(srcip));
                    PrintInet(AF_INET6, (const void *)GET_IPV6_SRC_ADDR(p),
                            dstip, sizeof(dstip));
                }
                sp = p->dp;
                dp = p->sp;
            }
            break;
        case LOG_DIR_FLOW_TOCLIENT:
            if ((PKT_IS_TOCLIENT(p))) {
                if (PKT_IS_IPV4(p)) {
                    PrintInet(AF_INET, (const void *)GET_IPV4_SRC_ADDR_PTR(p),
                            srcip, sizeof(srcip));
                    PrintInet(AF_INET, (const void *)GET_IPV4_DST_ADDR_PTR(p),
                            dstip, sizeof(dstip));
                } else if (PKT_IS_IPV6(p)) {
                    PrintInet(AF_INET6, (const void *)GET_IPV6_SRC_ADDR(p),
                            srcip, sizeof(srcip));
                    PrintInet(AF_INET6, (const void *)GET_IPV6_DST_ADDR(p),
                            dstip, sizeof(dstip));
                }
                sp = p->sp;
                dp = p->dp;
            } else {
                if (PKT_IS_IPV4(p)) {
                    PrintInet(AF_INET, (const void *)GET_IPV4_DST_ADDR_PTR(p),
                            srcip, sizeof(srcip));
                    PrintInet(AF_INET, (const void *)GET_IPV4_SRC_ADDR_PTR(p),
                            dstip, sizeof(dstip));
                } else if (PKT_IS_IPV6(p)) {
                    PrintInet(AF_INET6, (const void *)GET_IPV6_DST_ADDR(p),
                            srcip, sizeof(srcip));
                    PrintInet(AF_INET6, (const void *)GET_IPV6_SRC_ADDR(p),
                            dstip, sizeof(dstip));
                }
                sp = p->dp;
                dp = p->sp;
            }
            break;
        default:
            DEBUG_VALIDATE_BUG_ON(1);
            return;
    }

    if (SCProtoNameValid(IP_GET_IPPROTO(p)) == TRUE) {
        strlcpy(proto, known_proto[IP_GET_IPPROTO(p)], sizeof(proto));
    } else {
        snprintf(proto, sizeof(proto), "%03" PRIu32, IP_GET_IPPROTO(p));
    }

    strlcpy(addr->src_ip, srcip, JSON_ADDR_LEN);

    switch(p->proto) {
        case IPPROTO_ICMP:
            break;
        case IPPROTO_UDP:
        case IPPROTO_TCP:
        case IPPROTO_SCTP:
            addr->sp = sp;
            break;
    }

    strlcpy(addr->dst_ip, dstip, JSON_ADDR_LEN);

    switch(p->proto) {
        case IPPROTO_ICMP:
            break;
        case IPPROTO_UDP:
        case IPPROTO_TCP:
        case IPPROTO_SCTP:
            addr->dp = dp;
            break;
    }

    strlcpy(addr->proto, proto, JSON_PROTO_LEN);
}

/**
 * \brief Add five tuple from packet to JSON object
 *
 * \param p Packet
 * \param dir log direction (packet or flow)
 * \param js JSON object
 */
void JsonFiveTuple(const Packet *p, enum OutputJsonLogDirection dir, json_t *js)
{
    char srcip[46] = {0}, dstip[46] = {0};
    Port sp, dp;
    char proto[16];

    switch (dir) {
        case LOG_DIR_PACKET:
            if (PKT_IS_IPV4(p)) {
                PrintInet(AF_INET, (const void *)GET_IPV4_SRC_ADDR_PTR(p),
                        srcip, sizeof(srcip));
                PrintInet(AF_INET, (const void *)GET_IPV4_DST_ADDR_PTR(p),
                        dstip, sizeof(dstip));
            } else if (PKT_IS_IPV6(p)) {
                PrintInet(AF_INET6, (const void *)GET_IPV6_SRC_ADDR(p),
                        srcip, sizeof(srcip));
                PrintInet(AF_INET6, (const void *)GET_IPV6_DST_ADDR(p),
                        dstip, sizeof(dstip));
            } else {
                /* Not an IP packet so don't do anything */
                return;
            }
            sp = p->sp;
            dp = p->dp;
            break;
        case LOG_DIR_FLOW:
        case LOG_DIR_FLOW_TOSERVER:
            if ((PKT_IS_TOSERVER(p))) {
                if (PKT_IS_IPV4(p)) {
                    PrintInet(AF_INET, (const void *)GET_IPV4_SRC_ADDR_PTR(p),
                            srcip, sizeof(srcip));
                    PrintInet(AF_INET, (const void *)GET_IPV4_DST_ADDR_PTR(p),
                            dstip, sizeof(dstip));
                } else if (PKT_IS_IPV6(p)) {
                    PrintInet(AF_INET6, (const void *)GET_IPV6_SRC_ADDR(p),
                            srcip, sizeof(srcip));
                    PrintInet(AF_INET6, (const void *)GET_IPV6_DST_ADDR(p),
                            dstip, sizeof(dstip));
                }
                sp = p->sp;
                dp = p->dp;
            } else {
                if (PKT_IS_IPV4(p)) {
                    PrintInet(AF_INET, (const void *)GET_IPV4_DST_ADDR_PTR(p),
                            srcip, sizeof(srcip));
                    PrintInet(AF_INET, (const void *)GET_IPV4_SRC_ADDR_PTR(p),
                            dstip, sizeof(dstip));
                } else if (PKT_IS_IPV6(p)) {
                    PrintInet(AF_INET6, (const void *)GET_IPV6_DST_ADDR(p),
                            srcip, sizeof(srcip));
                    PrintInet(AF_INET6, (const void *)GET_IPV6_SRC_ADDR(p),
                            dstip, sizeof(dstip));
                }
                sp = p->dp;
                dp = p->sp;
            }
            break;
        case LOG_DIR_FLOW_TOCLIENT:
            if ((PKT_IS_TOCLIENT(p))) {
                if (PKT_IS_IPV4(p)) {
                    PrintInet(AF_INET, (const void *)GET_IPV4_SRC_ADDR_PTR(p),
                            srcip, sizeof(srcip));
                    PrintInet(AF_INET, (const void *)GET_IPV4_DST_ADDR_PTR(p),
                            dstip, sizeof(dstip));
                } else if (PKT_IS_IPV6(p)) {
                    PrintInet(AF_INET6, (const void *)GET_IPV6_SRC_ADDR(p),
                            srcip, sizeof(srcip));
                    PrintInet(AF_INET6, (const void *)GET_IPV6_DST_ADDR(p),
                            dstip, sizeof(dstip));
                }
                sp = p->sp;
                dp = p->dp;
            } else {
                if (PKT_IS_IPV4(p)) {
                    PrintInet(AF_INET, (const void *)GET_IPV4_DST_ADDR_PTR(p),
                            srcip, sizeof(srcip));
                    PrintInet(AF_INET, (const void *)GET_IPV4_SRC_ADDR_PTR(p),
                            dstip, sizeof(dstip));
                } else if (PKT_IS_IPV6(p)) {
                    PrintInet(AF_INET6, (const void *)GET_IPV6_DST_ADDR(p),
                            srcip, sizeof(srcip));
                    PrintInet(AF_INET6, (const void *)GET_IPV6_SRC_ADDR(p),
                            dstip, sizeof(dstip));
                }
                sp = p->dp;
                dp = p->sp;
            }
            break;
        default:
            DEBUG_VALIDATE_BUG_ON(1);
            return;
    }

    if (SCProtoNameValid(IP_GET_IPPROTO(p)) == TRUE) {
        strlcpy(proto, known_proto[IP_GET_IPPROTO(p)], sizeof(proto));
    } else {
        snprintf(proto, sizeof(proto), "%03" PRIu32, IP_GET_IPPROTO(p));
    }

    json_object_set_new(js, "src_ip", json_string(srcip));

    switch(p->proto) {
        case IPPROTO_ICMP:
            break;
        case IPPROTO_UDP:
        case IPPROTO_TCP:
        case IPPROTO_SCTP:
            json_object_set_new(js, "src_port", json_integer(sp));
            break;
    }

    json_object_set_new(js, "dest_ip", json_string(dstip));

    switch(p->proto) {
        case IPPROTO_ICMP:
            break;
        case IPPROTO_UDP:
        case IPPROTO_TCP:
        case IPPROTO_SCTP:
            json_object_set_new(js, "dest_port", json_integer(dp));
            break;
    }

    json_object_set_new(js, "proto", json_string(proto));
}

#define COMMUNITY_ID_BUF_SIZE 64

static bool CalculateCommunityFlowIdv4(const Flow *f,
        const uint16_t seed, unsigned char *base64buf)
{
    struct {
        uint16_t seed;
        uint32_t src;
        uint32_t dst;
        uint8_t proto;
        uint8_t pad0;
        uint16_t sp;
        uint16_t dp;
    } __attribute__((__packed__)) ipv4;

    uint32_t src = f->src.addr_data32[0];
    uint32_t dst = f->dst.addr_data32[0];
    uint16_t sp = f->sp;
    if (f->proto == IPPROTO_ICMP)
        sp = f->icmp_s.type;
    sp = htons(sp);
    uint16_t dp = f->dp;
    if (f->proto == IPPROTO_ICMP)
        dp = f->icmp_d.type;
    dp = htons(dp);

    ipv4.seed = htons(seed);
    if (ntohl(src) < ntohl(dst) || (src == dst && sp < dp)) {
        ipv4.src = src;
        ipv4.dst = dst;
        ipv4.sp = sp;
        ipv4.dp = dp;
    } else {
        ipv4.src = dst;
        ipv4.dst = src;
        ipv4.sp = dp;
        ipv4.dp = sp;
    }
    ipv4.proto = f->proto;
    ipv4.pad0 = 0;

    uint8_t hash[20];
    if (ComputeSHA1((const uint8_t *)&ipv4, sizeof(ipv4), hash, sizeof(hash)) == 1) {
        strlcpy((char *)base64buf, "1:", COMMUNITY_ID_BUF_SIZE);
        unsigned long out_len = COMMUNITY_ID_BUF_SIZE - 2;
        if (Base64Encode(hash, sizeof(hash), base64buf+2, &out_len) == SC_BASE64_OK) {
            return true;
        }
    }
    return false;
}

static inline bool FlowHashRawAddressIPv6LtU32(const uint32_t *a, const uint32_t *b)
{
    for (int i = 0; i < 4; i++) {
        if (a[i] < b[i])
            return true;
        if (a[i] > b[i])
            break;
    }

    return false;
}

static bool CalculateCommunityFlowIdv6(const Flow *f,
        const uint16_t seed, unsigned char *base64buf)
{
    struct {
        uint16_t seed;
        uint32_t src[4];
        uint32_t dst[4];
        uint8_t proto;
        uint8_t pad0;
        uint16_t sp;
        uint16_t dp;
    } __attribute__((__packed__)) ipv6;

    uint16_t sp = f->sp;
    if (f->proto == IPPROTO_ICMPV6)
        sp = f->icmp_s.type;
    sp = htons(sp);
    uint16_t dp = f->dp;
    if (f->proto == IPPROTO_ICMPV6)
        dp = f->icmp_d.type;
    dp = htons(dp);

    ipv6.seed = htons(seed);
    if (FlowHashRawAddressIPv6LtU32(f->src.addr_data32, f->dst.addr_data32) ||
            ((memcmp(&f->src, &f->dst, sizeof(f->src)) == 0) && sp < dp))
    {
        memcpy(&ipv6.src, &f->src.addr_data32, 16);
        memcpy(&ipv6.dst, &f->dst.addr_data32, 16);
        ipv6.sp = sp;
        ipv6.dp = dp;
    } else {
        memcpy(&ipv6.src, &f->dst.addr_data32, 16);
        memcpy(&ipv6.dst, &f->src.addr_data32, 16);
        ipv6.sp = dp;
        ipv6.dp = sp;
    }
    ipv6.proto = f->proto;
    ipv6.pad0 = 0;

    uint8_t hash[20];
    if (ComputeSHA1((const uint8_t *)&ipv6, sizeof(ipv6), hash, sizeof(hash)) == 1) {
        strlcpy((char *)base64buf, "1:", COMMUNITY_ID_BUF_SIZE);
        unsigned long out_len = COMMUNITY_ID_BUF_SIZE - 2;
        if (Base64Encode(hash, sizeof(hash), base64buf+2, &out_len) == SC_BASE64_OK) {
            return true;
        }
    }
    return false;
}

static void CreateJSONCommunityFlowId(json_t *js, const Flow *f, const uint16_t seed)
{
    unsigned char buf[COMMUNITY_ID_BUF_SIZE];
    if (f->flags & FLOW_IPV4) {
        if (CalculateCommunityFlowIdv4(f, seed, buf)) {
            json_object_set_new(js, "community_id", json_string((const char *)buf));
        }
    } else if (f->flags & FLOW_IPV6) {
        if (CalculateCommunityFlowIdv6(f, seed, buf)) {
            json_object_set_new(js, "community_id", json_string((const char *)buf));
        }
    }
}

static void CreateEveCommunityFlowId(JsonBuilder *js, const Flow *f, const uint16_t seed)
{
    unsigned char buf[COMMUNITY_ID_BUF_SIZE];
    if (f->flags & FLOW_IPV4) {
        if (CalculateCommunityFlowIdv4(f, seed, buf)) {
            jb_set_string(js, "community_id", (const char *)buf);
        }
    } else if (f->flags & FLOW_IPV6) {
        if (CalculateCommunityFlowIdv6(f, seed, buf)) {
            jb_set_string(js, "community_id", (const char *)buf);
        }
    }
}

void CreateJSONFlowId(json_t *js, const Flow *f)
{
    if (f == NULL)
        return;
    int64_t flow_id = FlowGetId(f);
    json_object_set_new(js, "flow_id", json_integer(flow_id));
    if (f->parent_id) {
        json_object_set_new(js, "parent_id", json_integer(f->parent_id));
    }
}

void CreateEveFlowId(JsonBuilder *js, const Flow *f)
{
    if (f == NULL) {
        return;
    }
    int64_t flow_id = FlowGetId(f);
    jb_set_uint(js, "flow_id", flow_id);
    if (f->parent_id) {
        jb_set_uint(js, "parent_id", f->parent_id);
    }
}

json_t *CreateJSONHeader(const Packet *p, enum OutputJsonLogDirection dir,
                         const char *event_type, JsonAddrInfo *addr)
{
    char timebuf[64];
    const Flow *f = (const Flow *)p->flow;

    json_t *js = json_object();
    if (unlikely(js == NULL))
        return NULL;

    CreateIsoTimeString(&p->ts, timebuf, sizeof(timebuf));

    /* time & tx */
    json_object_set_new(js, "timestamp", json_string(timebuf));

    CreateJSONFlowId(js, f);

    /* sensor id */
    if (sensor_id >= 0)
        json_object_set_new(js, "sensor_id", json_integer(sensor_id));

    /* input interface */
    if (p->livedev) {
        json_object_set_new(js, "in_iface", json_string(p->livedev->dev));
    }

    /* pcap_cnt */
    if (p->pcap_cnt != 0) {
        json_object_set_new(js, "pcap_cnt", json_integer(p->pcap_cnt));
    }

    if (event_type) {
        json_object_set_new(js, "event_type", json_string(event_type));
    }

    /* vlan */
    if (p->vlan_idx > 0) {
        json_t *js_vlan = json_array();
        if (js_vlan) {
            json_array_append_new(js_vlan, json_integer(p->vlan_id[0]));
            if (p->vlan_idx > 1) {
                json_array_append_new(js_vlan, json_integer(p->vlan_id[1]));
            }
            json_object_set_new(js, "vlan", js_vlan);
        }
    }

    /* 5-tuple */
    JsonAddrInfo addr_info = json_addr_info_zero;
    if (addr == NULL) {
        JsonAddrInfoInit(p, dir, &addr_info);
        addr = &addr_info;
    }
    json_object_set_new(js, "src_ip", json_string(addr->src_ip));
    json_object_set_new(js, "src_port", json_integer(addr->sp));
    json_object_set_new(js, "dest_ip", json_string(addr->dst_ip));
    json_object_set_new(js, "dest_port", json_integer(addr->dp));
    json_object_set_new(js, "proto", json_string(addr->proto));

    /* icmp */
    switch (p->proto) {
        case IPPROTO_ICMP:
            if (p->icmpv4h) {
                json_object_set_new(js, "icmp_type",
                                    json_integer(p->icmpv4h->type));
                json_object_set_new(js, "icmp_code",
                                    json_integer(p->icmpv4h->code));
            }
            break;
        case IPPROTO_ICMPV6:
            if (p->icmpv6h) {
                json_object_set_new(js, "icmp_type",
                                    json_integer(p->icmpv6h->type));
                json_object_set_new(js, "icmp_code",
                                    json_integer(p->icmpv6h->code));
            }
            break;
    }

    return js;
}

JsonBuilder *CreateEveHeader(const Packet *p, enum OutputJsonLogDirection dir,
        const char *event_type, JsonAddrInfo *addr)
{
    char timebuf[64];
    const Flow *f = (const Flow *)p->flow;

    JsonBuilder *js = jb_new_object();
    if (unlikely(js == NULL)) {
        return NULL;
    }

    CreateIsoTimeString(&p->ts, timebuf, sizeof(timebuf));

    jb_set_string(js, "timestamp", timebuf);

    CreateEveFlowId(js, f);

    /* sensor id */
    if (sensor_id >= 0) {
        jb_set_uint(js, "sensor_id", sensor_id);
    }

    /* input interface */
    if (p->livedev) {
        jb_set_string(js, "in_iface", p->livedev->dev);
    }

    /* pcap_cnt */
    if (p->pcap_cnt != 0) {
        jb_set_uint(js, "pcap_cnt", p->pcap_cnt);
    }

    if (event_type) {
        jb_set_string(js, "event_type", event_type);
    }

    /* vlan */
    if (p->vlan_idx > 0) {
        jb_open_array(js, "vlan");
        jb_append_uint(js, p->vlan_id[0]);
        if (p->vlan_idx > 1) {
            jb_append_uint(js, p->vlan_id[1]);
        }
        jb_close(js);
    }

    /* 5-tuple */
    JsonAddrInfo addr_info = json_addr_info_zero;
    if (addr == NULL) {
        JsonAddrInfoInit(p, dir, &addr_info);
        addr = &addr_info;
    }
    jb_set_string(js, "src_ip", addr->src_ip);
    jb_set_uint(js, "src_port", addr->sp);
    jb_set_string(js, "dest_ip", addr->dst_ip);
    jb_set_uint(js, "dest_port", addr->dp);
    jb_set_string(js, "proto", addr->proto);

    /* icmp */
    switch (p->proto) {
        case IPPROTO_ICMP:
            if (p->icmpv4h) {
                jb_set_uint(js, "icmp_type", p->icmpv4h->type);
                jb_set_uint(js, "icmp_code", p->icmpv4h->code);
            }
            break;
        case IPPROTO_ICMPV6:
            if (p->icmpv6h) {
                jb_set_uint(js, "icmp_type", p->icmpv6h->type);
                jb_set_uint(js, "icmp_code", p->icmpv6h->code);
            }
            break;
    }

    return js;
}

json_t *CreateJSONHeaderWithTxId(const Packet *p, enum OutputJsonLogDirection dir,
                                 const char *event_type, uint64_t tx_id)
{
    json_t *js = CreateJSONHeader(p, dir, event_type, NULL);
    if (unlikely(js == NULL))
        return NULL;

    /* tx id for correlation with other events */
    json_object_set_new(js, "tx_id", json_integer(tx_id));

    return js;
}

JsonBuilder *CreateEveHeaderWithTxId(const Packet *p, enum OutputJsonLogDirection dir,
                                 const char *event_type, JsonAddrInfo *addr, uint64_t tx_id)
{
    JsonBuilder *js = CreateEveHeader(p, dir, event_type, addr);
    if (unlikely(js == NULL))
        return NULL;

    /* tx id for correlation with other events */
    jb_set_uint(js, "tx_id", tx_id);

    return js;
}

int OutputJSONMemBufferCallback(const char *str, size_t size, void *data)
{
    OutputJSONMemBufferWrapper *wrapper = data;
    MemBuffer **memb = wrapper->buffer;

    if (MEMBUFFER_OFFSET(*memb) + size >= MEMBUFFER_SIZE(*memb)) {
        MemBufferExpand(memb, wrapper->expand_by);
    }

    MemBufferWriteRaw((*memb), str, size);
    return 0;
}

int OutputJSONBuffer(json_t *js, LogFileCtx *file_ctx, MemBuffer **buffer)
{
    if (file_ctx->sensor_name) {
        json_object_set_new(js, "host",
                            json_string(file_ctx->sensor_name));
    }

    if (file_ctx->is_pcap_offline) {
        json_object_set_new(js, "pcap_filename", json_string(PcapFileGetFilename()));
    }

    if (file_ctx->prefix) {
        MemBufferWriteRaw((*buffer), file_ctx->prefix, file_ctx->prefix_len);
    }

    OutputJSONMemBufferWrapper wrapper = {
        .buffer = buffer,
        .expand_by = JSON_OUTPUT_BUFFER_SIZE
    };

    int r = json_dump_callback(js, OutputJSONMemBufferCallback, &wrapper,
            file_ctx->json_flags);
    if (r != 0)
        return TM_ECODE_OK;

    LogFileWrite(file_ctx, *buffer);
    return 0;
}

int OutputJsonBuilderBuffer(JsonBuilder *js, LogFileCtx *file_ctx, MemBuffer **buffer)
{
    if (file_ctx->sensor_name) {
        jb_set_string(js, "host", file_ctx->sensor_name);
    }

    if (file_ctx->is_pcap_offline) {
        jb_set_string(js, "pcap_filename", PcapFileGetFilename());
    }

    jb_close(js);

    if (file_ctx->prefix) {
        MemBufferWriteRaw((*buffer), file_ctx->prefix, file_ctx->prefix_len);
    }

    size_t jslen = jb_len(js);
    if (MEMBUFFER_OFFSET(*buffer) + jslen >= MEMBUFFER_SIZE(*buffer)) {
        MemBufferExpand(buffer, jslen);
    }

    MemBufferWriteRaw((*buffer), jb_ptr(js), jslen);
    LogFileWrite(file_ctx, *buffer);

    return 0;
}

/**
 * \brief Create a new LogFileCtx for "fast" output style.
 * \param conf The configuration node for this output.
 * \return A LogFileCtx pointer on success, NULL on failure.
 */
OutputInitResult OutputJsonInitCtx(ConfNode *conf)
{
    OutputInitResult result = { NULL, false };

    OutputJsonCtx *json_ctx = SCCalloc(1, sizeof(OutputJsonCtx));
    if (unlikely(json_ctx == NULL)) {
        SCLogDebug("could not create new OutputJsonCtx");
        return result;
    }

    /* First lookup a sensor-name value in this outputs configuration
     * node (deprecated). If that fails, lookup the global one. */
    const char *sensor_name = ConfNodeLookupChildValue(conf, "sensor-name");
    if (sensor_name != NULL) {
        SCLogWarning(SC_ERR_DEPRECATED_CONF,
            "Found deprecated eve-log setting \"sensor-name\". "
            "Please set sensor-name globally.");
    }
    else {
        (void)ConfGet("sensor-name", &sensor_name);
    }

    json_ctx->file_ctx = LogFileNewCtx();
    if (unlikely(json_ctx->file_ctx == NULL)) {
        SCLogDebug("AlertJsonInitCtx: Could not create new LogFileCtx");
        SCFree(json_ctx);
        return result;
    }

    if (sensor_name) {
        json_ctx->file_ctx->sensor_name = SCStrdup(sensor_name);
        if (json_ctx->file_ctx->sensor_name  == NULL) {
            LogFileFreeCtx(json_ctx->file_ctx);
            SCFree(json_ctx);
            return result;
        }
    } else {
        json_ctx->file_ctx->sensor_name = NULL;
    }

    OutputCtx *output_ctx = SCCalloc(1, sizeof(OutputCtx));
    if (unlikely(output_ctx == NULL)) {
        LogFileFreeCtx(json_ctx->file_ctx);
        SCFree(json_ctx);
        return result;
    }

    output_ctx->data = json_ctx;
    output_ctx->DeInit = OutputJsonDeInitCtx;

    if (conf) {
        const char *output_s = ConfNodeLookupChildValue(conf, "filetype");

        // Backwards compatibility
        if (output_s == NULL) {
            output_s = ConfNodeLookupChildValue(conf, "type");
        }

        if (output_s != NULL) {
            if (strcmp(output_s, "file") == 0 ||
                strcmp(output_s, "regular") == 0) {
                json_ctx->json_out = LOGFILE_TYPE_FILE;
            } else if (strcmp(output_s, "syslog") == 0) {
                json_ctx->json_out = LOGFILE_TYPE_SYSLOG;
            } else if (strcmp(output_s, "unix_dgram") == 0) {
                json_ctx->json_out = LOGFILE_TYPE_UNIX_DGRAM;
            } else if (strcmp(output_s, "unix_stream") == 0) {
                json_ctx->json_out = LOGFILE_TYPE_UNIX_STREAM;
            } else if (strcmp(output_s, "redis") == 0) {
#ifdef HAVE_LIBHIREDIS
                SCLogRedisInit();
                json_ctx->json_out = LOGFILE_TYPE_REDIS;
#else
                SCLogError(SC_ERR_INVALID_ARGUMENT,
                           "redis JSON output option is not compiled");
                exit(EXIT_FAILURE);
#endif
            } else {
                SCLogError(SC_ERR_INVALID_ARGUMENT,
                           "Invalid JSON output option: %s", output_s);
                exit(EXIT_FAILURE);
            }
        }

        const char *prefix = ConfNodeLookupChildValue(conf, "prefix");
        if (prefix != NULL)
        {
            SCLogInfo("Using prefix '%s' for JSON messages", prefix);
            json_ctx->file_ctx->prefix = SCStrdup(prefix);
            if (json_ctx->file_ctx->prefix == NULL)
            {
                SCLogError(SC_ERR_MEM_ALLOC,
                    "Failed to allocate memory for eve-log.prefix setting.");
                exit(EXIT_FAILURE);
            }
            json_ctx->file_ctx->prefix_len = strlen(prefix);
        }

        if (json_ctx->json_out == LOGFILE_TYPE_FILE ||
            json_ctx->json_out == LOGFILE_TYPE_UNIX_DGRAM ||
            json_ctx->json_out == LOGFILE_TYPE_UNIX_STREAM)
        {
            if (SCConfLogOpenGeneric(conf, json_ctx->file_ctx, DEFAULT_LOG_FILENAME, 1) < 0) {
                LogFileFreeCtx(json_ctx->file_ctx);
                SCFree(json_ctx);
                SCFree(output_ctx);
                return result;
            }
            OutputRegisterFileRotationFlag(&json_ctx->file_ctx->rotation_flag);
        }
#ifndef OS_WIN32
	else if (json_ctx->json_out == LOGFILE_TYPE_SYSLOG) {
            const char *facility_s = ConfNodeLookupChildValue(conf, "facility");
            if (facility_s == NULL) {
                facility_s = DEFAULT_ALERT_SYSLOG_FACILITY_STR;
            }

            int facility = SCMapEnumNameToValue(facility_s, SCSyslogGetFacilityMap());
            if (facility == -1) {
                SCLogWarning(SC_ERR_INVALID_ARGUMENT, "Invalid syslog facility: \"%s\","
                        " now using \"%s\" as syslog facility", facility_s,
                        DEFAULT_ALERT_SYSLOG_FACILITY_STR);
                facility = DEFAULT_ALERT_SYSLOG_FACILITY;
            }

            const char *level_s = ConfNodeLookupChildValue(conf, "level");
            if (level_s != NULL) {
                int level = SCMapEnumNameToValue(level_s, SCSyslogGetLogLevelMap());
                if (level != -1) {
                    json_ctx->file_ctx->syslog_setup.alert_syslog_level = level;
                }
            }

            const char *ident = ConfNodeLookupChildValue(conf, "identity");
            /* if null we just pass that to openlog, which will then
             * figure it out by itself. */

            openlog(ident, LOG_PID|LOG_NDELAY, facility);
        }
#endif
#ifdef HAVE_LIBHIREDIS
        else if (json_ctx->json_out == LOGFILE_TYPE_REDIS) {
            ConfNode *redis_node = ConfNodeLookupChild(conf, "redis");
            if (!json_ctx->file_ctx->sensor_name) {
                char hostname[1024];
                gethostname(hostname, 1023);
                json_ctx->file_ctx->sensor_name = SCStrdup(hostname);
            }
            if (json_ctx->file_ctx->sensor_name  == NULL) {
                LogFileFreeCtx(json_ctx->file_ctx);
                SCFree(json_ctx);
                SCFree(output_ctx);
                return result;
            }

            if (SCConfLogOpenRedis(redis_node, json_ctx->file_ctx) < 0) {
                LogFileFreeCtx(json_ctx->file_ctx);
                SCFree(json_ctx);
                SCFree(output_ctx);
                return result;
            }
        }
#endif

        const char *sensor_id_s = ConfNodeLookupChildValue(conf, "sensor-id");
        if (sensor_id_s != NULL) {
            if (StringParseUint64((uint64_t *)&sensor_id, 10, 0, sensor_id_s) < 0) {
                SCLogError(SC_ERR_INVALID_ARGUMENT,
                           "Failed to initialize JSON output, "
                           "invalid sensor-id: %s", sensor_id_s);
                exit(EXIT_FAILURE);
            }
        }

        /* Check if top-level metadata should be logged. */
        const ConfNode *metadata = ConfNodeLookupChild(conf, "metadata");
        if (metadata && metadata->val && ConfValIsFalse(metadata->val)) {
            SCLogConfig("Disabling eve metadata logging.");
            json_ctx->cfg.include_metadata = false;
        } else {
            json_ctx->cfg.include_metadata = true;
        }

        /* See if we want to enable the community id */
        const ConfNode *community_id = ConfNodeLookupChild(conf, "community-id");
        if (community_id && community_id->val && ConfValIsTrue(community_id->val)) {
            SCLogConfig("Enabling eve community_id logging.");
            json_ctx->cfg.include_community_id = true;
        } else {
            json_ctx->cfg.include_community_id = false;
        }
        const char *cid_seed = ConfNodeLookupChildValue(conf, "community-id-seed");
        if (cid_seed != NULL) {
            if (StringParseUint16(&json_ctx->cfg.community_id_seed,
                        10, 0, cid_seed) < 0)
            {
                SCLogError(SC_ERR_INVALID_ARGUMENT,
                           "Failed to initialize JSON output, "
                           "invalid community-id-seed: %s", cid_seed);
                exit(EXIT_FAILURE);
            }
        }

        /* Do we have a global eve xff configuration? */
        const ConfNode *xff = ConfNodeLookupChild(conf, "xff");
        if (xff != NULL) {
            json_ctx->xff_cfg = SCCalloc(1, sizeof(HttpXFFCfg));
            if (likely(json_ctx->xff_cfg != NULL)) {
                HttpXFFGetCfg(conf, json_ctx->xff_cfg);
            }
        }

        const char *pcapfile_s = ConfNodeLookupChildValue(conf, "pcap-file");
        if (pcapfile_s != NULL && ConfValIsTrue(pcapfile_s)) {
            json_ctx->file_ctx->is_pcap_offline =
                (RunmodeGetCurrent() == RUNMODE_PCAP_FILE ||
                 RunmodeGetCurrent() == RUNMODE_UNIX_SOCKET);
        }

        json_ctx->file_ctx->type = json_ctx->json_out;
    }


    SCLogDebug("returning output_ctx %p", output_ctx);

    result.ctx = output_ctx;
    result.ok = true;
    return result;
}

static void OutputJsonDeInitCtx(OutputCtx *output_ctx)
{
    OutputJsonCtx *json_ctx = (OutputJsonCtx *)output_ctx->data;
    LogFileCtx *logfile_ctx = json_ctx->file_ctx;
    if (logfile_ctx->dropped) {
        SCLogWarning(SC_WARN_EVENT_DROPPED,
                "%"PRIu64" events were dropped due to slow or "
                "disconnected socket", logfile_ctx->dropped);
    }
    if (json_ctx->xff_cfg != NULL) {
        SCFree(json_ctx->xff_cfg);
    }
    LogFileFreeCtx(logfile_ctx);
    SCFree(json_ctx);
    SCFree(output_ctx);
}
