/* PAT, PMT, SDT actual and NIT actual of a TS, see psi.h. */
#include "psi.h"

#include <stdlib.h>
#include <string.h>

#define MAX_SECTIONS 512

struct asm_buf {
    uint8_t buf[4096 + 188];
    int len, active;
};

struct section {
    int table, ext, num, len;
    uint8_t* data;
};

struct psi {
    uint32_t npk[8192], nscr[8192];
    uint8_t want[8192];
    struct asm_buf* as[8192];
    struct section sec[MAX_SECTIONS];
    int nsec;
};

static uint32_t crc32_mpeg(const uint8_t* d, int n)
{
    uint32_t c = 0xFFFFFFFF;
    for (int i = 0; i < n; i++) {
        c ^= (uint32_t)d[i] << 24;
        for (int k = 0; k < 8; k++)
            c = c & 0x80000000 ? (c << 1) ^ 0x04C11DB7 : c << 1;
    }
    return c;
}

psi* psi_new(void)
{
    psi* p = calloc(1, sizeof(*p));
    if (p)
        p->want[0x00] = p->want[0x10] = p->want[0x11] = 1;
    return p;
}

void psi_free(psi* p)
{
    if (!p)
        return;
    for (int i = 0; i < 8192; i++)
        free(p->as[i]);
    for (int i = 0; i < p->nsec; i++)
        free(p->sec[i].data);
    free(p);
}

static const struct section* find(const psi* p, int table, int ext, int num)
{
    for (int i = 0; i < p->nsec; i++)
        if (p->sec[i].table == table && p->sec[i].ext == ext && p->sec[i].num == num)
            return &p->sec[i];
    return NULL;
}

/* A complete section: kept once (per table, extension, number) when its
 * CRC is right; the PAT marks the PMT PIDs to collect. */
static void section_done(psi* p, const uint8_t* s, int len)
{
    if (len < 12 || !(s[1] & 0x80) || crc32_mpeg(s, len) != 0)
        return;
    const int table = s[0], ext = s[3] << 8 | s[4], num = s[6];
    if (table != 0x00 && table != 0x02 && table != 0x40 && table != 0x42)
        return;
    if (find(p, table, ext, num) || p->nsec == MAX_SECTIONS)
        return;
    struct section* x = &p->sec[p->nsec];
    x->data = malloc(len);
    if (!x->data)
        return;
    memcpy(x->data, s, len);
    x->table = table;
    x->ext = ext;
    x->num = num;
    x->len = len;
    p->nsec++;
    if (table == 0x00)
        for (int j = 8; j + 4 <= len - 4; j += 4) {
            const int prog = s[j] << 8 | s[j + 1], pid = (s[j + 2] & 0x1f) << 8 | s[j + 3];
            if (prog)
                p->want[pid] = 1;
        }
}

static void assemble(psi* p, int pid, const uint8_t* pl, int n, int pusi)
{
    struct asm_buf* a = p->as[pid];
    if (!a && !(a = p->as[pid] = calloc(1, sizeof(*a))))
        return;
    if (pusi) {
        const int off = pl[0];
        if (off + 1 > n)
            return;
        if (a->active && off) { /* end of the previous section */
            const int m = off < (int)sizeof(a->buf) - a->len ? off : (int)sizeof(a->buf) - a->len;
            memcpy(a->buf + a->len, pl + 1, m);
            a->len += m;
        }
        if (a->active && a->len >= 3) {
            const int L = ((a->buf[1] & 0xf) << 8 | a->buf[2]) + 3;
            if (a->len >= L)
                section_done(p, a->buf, L);
        }
        pl += 1 + off;
        n -= 1 + off;
        a->len = 0;
        a->active = 1;
    } else if (!a->active) {
        return;
    }
    int m = n < (int)sizeof(a->buf) - a->len ? n : (int)sizeof(a->buf) - a->len;
    memcpy(a->buf + a->len, pl, m);
    a->len += m;
    /* complete sections in the buffer (several may share a packet) */
    for (;;) {
        if (a->len < 3 || a->buf[0] == 0xff) {
            if (a->len && a->buf[0] == 0xff)
                a->active = 0;
            return;
        }
        const int L = ((a->buf[1] & 0xf) << 8 | a->buf[2]) + 3;
        if (a->len < L)
            return;
        section_done(p, a->buf, L);
        memmove(a->buf, a->buf + L, a->len - L);
        a->len -= L;
    }
}

void psi_feed(psi* p, const uint8_t* ts, int len)
{
    for (int i = 0; i + 188 <= len; i += 188) {
        const uint8_t* k = ts + i;
        if (k[0] != 0x47 || (k[1] & 0x80))
            continue;
        const int pid = (k[1] & 0x1f) << 8 | k[2];
        p->npk[pid]++;
        if (k[3] >> 6)
            p->nscr[pid]++;
        if (!p->want[pid] || (k[3] >> 6) || !(k[3] & 0x10))
            continue;
        const uint8_t* pl = k + 4;
        int n = 184;
        if (k[3] & 0x20) {
            n -= 1 + k[4];
            pl += 1 + k[4];
            if (n <= 0)
                continue;
        }
        assemble(p, pid, pl, n, k[1] & 0x40);
    }
}

/* DVB string to UTF-8: character table selector skipped (ISO 6937/8859
 * read as Latin-1, UTF-8 kept), control codes 0x80-0x9f dropped. */
static void dvb_text(const uint8_t* s, int n, char* out, int cap)
{
    int utf8 = 0, o = 0;
    if (n > 0 && s[0] < 0x20) {
        if (s[0] == 0x15)
            utf8 = 1;
        const int skip = s[0] == 0x10 ? 3 : s[0] == 0x1f ? 2 : 1;
        s += skip;
        n -= skip;
    }
    for (int i = 0; i < n && o < cap - 3; i++) {
        const uint8_t c = s[i];
        if (c < 0x20 || (c >= 0x80 && c < 0xa0 && !utf8) || c == '"' || c == '\\')
            continue;
        if (c < 0x80 || utf8) {
            out[o++] = (char)c;
        } else {
            out[o++] = (char)(0xc0 | c >> 6);
            out[o++] = (char)(0x80 | (c & 0x3f));
        }
    }
    out[o] = 0;
}

static uint32_t bcd(const uint8_t* b, int digits)
{
    uint32_t v = 0;
    for (int i = 0; i < digits; i++)
        v = v * 10 + ((b[i / 2] >> (i & 1 ? 0 : 4)) & 0xf);
    return v;
}

static int is_av(int stream_type)
{
    switch (stream_type) {
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x06: case 0x0f: case 0x11:
    case 0x1b: case 0x24: case 0x81: case 0x87:
        return 1;
    }
    return 0;
}

static void nit_mux(struct psi_result* r, const struct psi_mux* m)
{
    for (int i = 0; i < r->nmuxes; i++)
        if (r->muxes[i].tsid == m->tsid && (int)(r->muxes[i].freq / 1e3) == (int)(m->freq / 1e3))
            return;
    if (r->nmuxes < PSI_MAX_MUXES)
        r->muxes[r->nmuxes++] = *m;
}

void psi_result_get(const psi* p, struct psi_result* r)
{
    memset(r, 0, sizeof(*r));
    r->tsid = r->onid = -1;
    for (int i = 0; i < p->nsec; i++) {
        const struct section* x = &p->sec[i];
        const uint8_t* s = x->data;
        const int end = x->len - 4;
        if (x->table == 0x00) {
            r->tsid = x->ext;
            for (int j = 8; j + 4 <= end; j += 4) {
                const int prog = s[j] << 8 | s[j + 1];
                if (!prog || r->nservices == PSI_MAX_SERVICES)
                    continue;
                int k = 0;
                while (k < r->nservices && r->services[k].sid != prog)
                    k++;
                if (k == r->nservices) {
                    r->services[k] = (struct psi_service){ .sid = prog, .type = -1 };
                    r->nservices++;
                }
            }
        }
    }
    for (int i = 0; i < p->nsec; i++) {
        const struct section* x = &p->sec[i];
        const uint8_t* s = x->data;
        const int end = x->len - 4;
        if (x->table == 0x02) { /* PMT: scrambled audio/video */
            int k = 0;
            while (k < r->nservices && r->services[k].sid != x->ext)
                k++;
            if (k == r->nservices)
                continue;
            const int pil = (s[10] & 0xf) << 8 | s[11];
            uint64_t n = 0, sc = 0;
            for (int j = 12 + pil; j + 5 <= end;) {
                const int st = s[j], pid = (s[j + 1] & 0x1f) << 8 | s[j + 2];
                const int il = (s[j + 3] & 0xf) << 8 | s[j + 4];
                if (is_av(st)) {
                    n += p->npk[pid];
                    sc += p->nscr[pid];
                }
                j += 5 + il;
            }
            r->services[k].scrambled = n > 0 && 2 * sc > n;
        } else if (x->table == 0x42) { /* SDT actual */
            r->onid = s[8] << 8 | s[9];
            for (int j = 11; j + 5 <= end;) {
                const int sid = s[j] << 8 | s[j + 1], dl = (s[j + 3] & 0xf) << 8 | s[j + 4];
                int k = 0;
                while (k < r->nservices && r->services[k].sid != sid)
                    k++;
                if (k == r->nservices && r->nservices < PSI_MAX_SERVICES)
                    r->services[r->nservices++] = (struct psi_service){ .sid = sid, .type = -1 };
                if (k < r->nservices)
                    for (int d = j + 5; d + 2 <= j + 5 + dl && d + 2 <= end; d += 2 + s[d + 1]) {
                        if (s[d] != 0x48 || s[d + 1] < 3)
                            continue;
                        const uint8_t* v = s + d + 2;
                        const int pn = v[1];
                        if (2 + pn + 1 > s[d + 1])
                            continue;
                        const int nn = v[2 + pn];
                        r->services[k].type = v[0];
                        dvb_text(v + 2, pn, r->services[k].provider, sizeof(r->services[k].provider));
                        if (3 + pn + nn <= s[d + 1])
                            dvb_text(v + 3 + pn, nn, r->services[k].name, sizeof(r->services[k].name));
                    }
                j += 5 + dl;
            }
        } else if (x->table == 0x40) { /* NIT actual */
            const int ndl = (s[8] & 0xf) << 8 | s[9];
            for (int d = 10; d + 2 <= 10 + ndl && d + 2 <= end; d += 2 + s[d + 1])
                if (s[d] == 0x40)
                    dvb_text(s + d + 2, s[d + 1], r->network, sizeof(r->network));
            int j = 10 + ndl;
            if (j + 2 > end)
                continue;
            const int tl = (s[j] & 0xf) << 8 | s[j + 1];
            j += 2;
            const int tend = j + tl < end ? j + tl : end;
            while (j + 6 <= tend) {
                const int tsid = s[j] << 8 | s[j + 1], dl = (s[j + 4] & 0xf) << 8 | s[j + 5];
                for (int d = j + 6; d + 2 <= j + 6 + dl && d + 2 <= tend; d += 2 + s[d + 1]) {
                    const uint8_t* v = s + d + 2;
                    const int ln = s[d + 1];
                    struct psi_mux m = { .tsid = tsid, .plp = -1 };
                    if (s[d] == 0x44 && ln >= 11) { /* cable */
                        strcpy(m.delsys, "DVB-C");
                        m.freq = bcd(v, 8) * 100.0; /* xxxx.xxxx MHz */
                        static const int Q[] = { 0, 16, 32, 64, 128, 256 };
                        m.modulation = v[6] <= 5 ? Q[v[6]] : 0;
                        m.symbol_rate = bcd(v + 7, 7) * 100.0; /* xxx.xxxx Msym/s */
                        nit_mux(r, &m);
                    } else if (s[d] == 0x43 && ln >= 11) { /* satellite */
                        m.freq = bcd(v, 8) * 10e3; /* xxx.xxxxx GHz */
                        m.polarization = "HVLR"[(v[6] >> 5) & 3];
                        strcpy(m.delsys, v[6] & 0x04 ? "DVB-S2" : "DVB-S");
                        m.symbol_rate = bcd(v + 7, 7) * 100.0;
                        nit_mux(r, &m);
                    } else if (s[d] == 0x5a && ln >= 11) { /* terrestrial */
                        strcpy(m.delsys, "DVB-T");
                        const uint32_t cf = (uint32_t)v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3];
                        static const double B[] = { 8e6, 7e6, 6e6, 5e6 };
                        m.bandwidth = B[(v[4] >> 5) & 3];
                        if (cf != 0xFFFFFFFFu && cf) { /* all ones: unknown (SFN, frequency list) */
                            m.freq = cf * 10.0;
                            nit_mux(r, &m);
                        }
                    } else if (s[d] == 0x7f && ln >= 4 && v[0] == 0x04) { /* T2 delivery */
                        strcpy(m.delsys, "DVB-T2");
                        m.plp = v[1];
                        if (ln >= 6 + 6 && !(v[5] & 1)) { /* cell loop, no TFS */
                            static const double B[] = { 8e6, 7e6, 6e6, 5e6, 10e6, 1.712e6 };
                            const int bwc = (v[4] >> 2) & 0xf;
                            m.bandwidth = bwc < 6 ? B[bwc] : 8e6;
                            const uint8_t* c = v + 6;
                            m.freq = ((uint32_t)c[2] << 24 | c[3] << 16 | c[4] << 8 | c[5]) * 10.0;
                            nit_mux(r, &m);
                        }
                    }
                }
                j += 6 + dl;
            }
        }
    }
}

void psi_pid_count(const psi* p, int pid, uint32_t* packets, uint32_t* scrambled)
{
    *packets = p->npk[pid & 0x1fff];
    *scrambled = p->nscr[pid & 0x1fff];
}
