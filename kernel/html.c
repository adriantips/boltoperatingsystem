#include <stdint.h>
#include "html.h"
#include "kheap.h"
#include "string.h"

/* ===========================================================================
 *  HTML -> run-list flattener. A single forward pass: text is decoded and
 *  whitespace-collapsed into styled runs; tags toggle style / link / colour /
 *  alignment / indent state and emit line breaks; <img> emits an image run;
 *  <script>/<style> bodies and comments are dropped. Not a real parser (no
 *  tree, no error recovery) -- a pragmatic reader that copes with real pages.
 * ===========================================================================*/

#define SEG_MAX   1024
#define TITLE_MAX 96
#define FSTACK_MAX 48

static char lc(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

/* decode one UTF-8 sequence at s[0..n); return bytes consumed, codepoint in *cp */
static int utf8_decode(const unsigned char *s, uint32_t n, uint32_t *cp) {
    unsigned char c = s[0];
    if (c < 0x80) { *cp = c; return 1; }
    int len; uint32_t v;
    if      ((c & 0xE0) == 0xC0) { len = 2; v = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { len = 3; v = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { len = 4; v = c & 0x07; }
    else { *cp = 0xFFFD; return 1; }
    if ((uint32_t)len > n) { *cp = 0xFFFD; return 1; }
    for (int k = 1; k < len; k++) { if ((s[k] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; } v = (v << 6) | (s[k] & 0x3F); }
    *cp = v; return len;
}

/* fold a Unicode codepoint to a single ASCII byte: accents -> base letter,
 * smart punctuation -> its ASCII twin. Returns -1 to drop the char entirely
 * (CJK, symbols and anything we can't sensibly render with an 8x8 ASCII font). */
static int fold_cp(uint32_t cp) {
    if (cp < 0x80) return (int)cp;
    switch (cp) {
        case 0x00A0: return ' ';                                   /* nbsp */
        case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'';
        case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"';
        case 0x2013: case 0x2014: case 0x2212: case 0x2010: case 0x2011: return '-';
        case 0x2022: case 0x00B7: case 0x25AA: case 0x2023: return '*';
        case 0x2026: return '.';
    }
    /* Latin-1 Supplement + Latin Extended-A: strip the diacritic */
    static const char *acc =
        /* C0..FF */ "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
    if (cp >= 0x00C0 && cp <= 0x00FF) return acc[cp - 0x00C0];
    if (cp == 0x0152) return 'O'; if (cp == 0x0153) return 'o';    /* OE / oe */
    if (cp == 0x0160 || cp == 0x015A) return 'S'; if (cp == 0x0161 || cp == 0x015B) return 's';
    if (cp == 0x017D || cp == 0x0179) return 'Z'; if (cp == 0x017E || cp == 0x017A) return 'z';
    return -1;
}

static char *arena_push(html_doc *d, const char *s, uint32_t n) {
    if (d->arena_len + n + 1 > d->arena_cap) return 0;
    char *p = d->arena + d->arena_len;
    for (uint32_t i = 0; i < n; i++) p[i] = s[i];
    p[n] = 0;
    d->arena_len += n + 1;
    return p;
}

/* full run record so callers can set the extended fields */
static void push_full(html_doc *d, const char *text, uint32_t n, uint8_t style,
                      int link, uint8_t brk, uint32_t color, uint8_t align, uint8_t indent) {
    if (d->nruns >= d->runs_cap) return;
    char *t = arena_push(d, text, n);
    if (!t) return;
    html_run *r = &d->runs[d->nruns++];
    r->text = t; r->style = style; r->link = link; r->brk = brk;
    r->kind = HRUN_TEXT; r->align = align; r->indent = indent; r->color = color;
    r->img = -1; r->iw = r->ih = 0; r->pix = 0;
}

/* decode an entity starting at src[*i]=='&'; return decoded char, advance *i */
static int decode_entity(const char *src, uint32_t *i, uint32_t len) {
    uint32_t j = *i + 1, start = j;
    if (j < len && src[j] == '#') {                 /* numeric */
        j++;
        int base = 10, val = 0;
        if (j < len && (src[j] == 'x' || src[j] == 'X')) { base = 16; j++; }
        while (j < len && src[j] != ';' && j - start < 8) {
            char c = src[j];
            int dv = (c >= '0' && c <= '9') ? c - '0'
                   : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                   : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
            if (dv < 0 || dv >= base) { val = -1; break; }
            val = val * base + dv; j++;
        }
        if (val >= 0 && j < len && src[j] == ';') { *i = j + 1; return val < 128 ? val : '?'; }
        return -1;
    }
    /* named */
    char name[8]; uint32_t k = 0;
    while (j < len && src[j] != ';' && k < sizeof(name) - 1) name[k++] = lc(src[j++]);
    name[k] = 0;
    if (j >= len || src[j] != ';') return -1;
    int out = -1;
    if      (strcmp(name, "amp") == 0)  out = '&';
    else if (strcmp(name, "lt") == 0)   out = '<';
    else if (strcmp(name, "gt") == 0)   out = '>';
    else if (strcmp(name, "quot") == 0) out = '"';
    else if (strcmp(name, "apos") == 0) out = '\'';
    else if (strcmp(name, "nbsp") == 0) out = ' ';
    else if (strcmp(name, "copy") == 0 || strcmp(name, "reg") == 0) out = '?';
    else if (strcmp(name, "mdash") == 0 || strcmp(name, "ndash") == 0 ||
             strcmp(name, "minus") == 0) out = '-';
    else if (strcmp(name, "ldquo") == 0 || strcmp(name, "rdquo") == 0 ||
             strcmp(name, "bdquo") == 0) out = '"';
    else if (strcmp(name, "lsquo") == 0 || strcmp(name, "rsquo") == 0 ||
             strcmp(name, "sbquo") == 0) out = '\'';
    else if (strcmp(name, "hellip") == 0) out = '.';
    else if (strcmp(name, "bull") == 0 || strcmp(name, "middot") == 0) out = '*';
    else if (strcmp(name, "times") == 0) out = 'x';
    else if (strcmp(name, "deg") == 0)   out = '?';
    else if (name[0] && name[1]) {
        /* accented latin names: <base><accent>, e.g. eacute, uuml, ntilde */
        char b = name[0];
        if ((b>='a'&&b<='z') &&
            (strcmp(name+1,"acute")==0 || strcmp(name+1,"grave")==0 ||
             strcmp(name+1,"circ")==0  || strcmp(name+1,"uml")==0   ||
             strcmp(name+1,"tilde")==0 || strcmp(name+1,"ring")==0  ||
             strcmp(name+1,"cedil")==0 || strcmp(name+1,"slash")==0))
            out = b;
    }
    if (out >= 0) { *i = j + 1; return out; }
    return -1;
}

/* extract a quoted/bare attribute value (case-insensitive name) into out */
static int get_attr(const char *tag, const char *attr, char *out, uint32_t cap) {
    uint32_t al = strlen(attr);
    out[0] = 0;
    for (const char *p = tag; *p; p++) {
        uint32_t k = 0;
        while (k < al && lc(p[k]) == attr[k]) k++;
        if (k == al) {
            /* must be a token boundary before the name (not mid-identifier) */
            char pc = p == tag ? ' ' : p[-1];
            if ((pc>='a'&&pc<='z')||(pc>='A'&&pc<='Z')||(pc>='0'&&pc<='9')||pc=='-') continue;
            const char *q = p + al;
            while (*q == ' ' || *q == '\t') q++;
            if (*q != '=') continue;
            q++;
            while (*q == ' ' || *q == '\t') q++;
            char quote = 0;
            if (*q == '"' || *q == '\'') quote = *q++;
            uint32_t o = 0;
            while (*q && o < cap - 1) {
                if (quote && *q == quote) break;
                if (!quote && (*q == ' ' || *q == '>' || *q == '\t')) break;
                out[o++] = *q++;
            }
            out[o] = 0;
            return 1;
        }
    }
    return 0;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = lc(c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* parse a CSS/HTML colour token -> 0x1RRGGBB (set bit) or HCOL_NONE */
static uint32_t parse_color_token(const char *v) {
    while (*v == ' ') v++;
    if (v[0] == '#') {
        const char *h = v + 1;
        int n = 0; while (hexval(h[n]) >= 0) n++;
        if (n >= 6) return 0x1000000u | (uint32_t)(hexval(h[0])*16+hexval(h[1]))<<16
                                       | (uint32_t)(hexval(h[2])*16+hexval(h[3]))<<8
                                       | (uint32_t)(hexval(h[4])*16+hexval(h[5]));
        if (n >= 3) return 0x1000000u | (uint32_t)(hexval(h[0])*17)<<16
                                       | (uint32_t)(hexval(h[1])*17)<<8
                                       | (uint32_t)(hexval(h[2])*17);
        return HCOL_NONE;
    }
    struct { const char *n; uint32_t c; } named[] = {
        {"black",0x000000},{"white",0xFFFFFF},{"red",0xE04040},{"green",0x40C040},
        {"blue",0x5070E0},{"yellow",0xE0E040},{"orange",0xE0A040},{"purple",0xA060E0},
        {"gray",0x909090},{"grey",0x909090},{"silver",0xC0C0C0},{"navy",0x303080},
        {"teal",0x40A0A0},{"maroon",0x903030},{"lime",0x60E060},{"cyan",0x40D0E0},
        {"magenta",0xE040E0},{"pink",0xE090B0},{"gold",0xE0C040},{"brown",0xA07040},
    };
    char low[16]; uint32_t i = 0; while (v[i] && i < sizeof(low)-1) { low[i] = lc(v[i]); i++; } low[i] = 0;
    for (uint32_t k = 0; k < sizeof(named)/sizeof(named[0]); k++)
        if (strcmp(low, named[k].n) == 0) return 0x1000000u | named[k].c;
    return HCOL_NONE;
}

/* pull colour from color="" / style="color:..." in a tag body */
static uint32_t tag_color(const char *tag) {
    char val[64];
    if (get_attr(tag, "color", val, sizeof(val))) { uint32_t c = parse_color_token(val); if (c) return c; }
    if (get_attr(tag, "style", val, sizeof(val))) {
        for (char *p = val; *p; p++)
            if (lc(p[0])=='c'&&lc(p[1])=='o'&&lc(p[2])=='l'&&lc(p[3])=='o'&&lc(p[4])=='r'
                && (p == val || p[-1] == ' ' || p[-1] == ';')) {   /* not background-color */
                const char *q = p + 5; while (*q == ' ') q++;
                if (*q == ':') return parse_color_token(q + 1);
            }
    }
    return HCOL_NONE;
}

/* pull alignment from align="" / style="text-align:..." */
static uint8_t tag_align(const char *tag) {
    char val[48];
    const char *a = 0;
    if (get_attr(tag, "align", val, sizeof(val))) a = val;
    else if (get_attr(tag, "style", val, sizeof(val))) {
        for (char *p = val; *p; p++)
            if (lc(p[0])=='t'&&lc(p[1])=='e'&&lc(p[2])=='x'&&lc(p[3])=='t'&&p[4]=='-') {
                const char *q = p; while (*q && *q != ':') q++; if (*q == ':') { a = q + 1; break; }
            }
    }
    if (!a) return HALIGN_LEFT;
    while (*a == ' ') a++;
    if (lc(a[0])=='c') return HALIGN_CENTER;
    if (lc(a[0])=='r') return HALIGN_RIGHT;
    return HALIGN_LEFT;
}

static html_doc *doc_alloc(uint32_t len) {
    html_doc *d = (html_doc *)kmalloc(sizeof(*d));
    if (!d) return 0;
    memset(d, 0, sizeof(*d));
    d->arena_cap = len + 1024;
    d->runs_cap  = (int)(len / 3 + 64); if (d->runs_cap > 24000) d->runs_cap = 24000;
    d->hrefs_cap = (int)(len / 40 + 32); if (d->hrefs_cap > 4000) d->hrefs_cap = 4000;
    d->imgs_cap  = (int)(len / 80 + 16); if (d->imgs_cap > 1024) d->imgs_cap = 1024;
    d->arena = (char *)kmalloc(d->arena_cap);
    d->runs  = (html_run *)kmalloc((uint32_t)d->runs_cap * sizeof(html_run));
    d->hrefs = (char **)kmalloc((uint32_t)d->hrefs_cap * sizeof(char *));
    d->imgs  = (char **)kmalloc((uint32_t)d->imgs_cap * sizeof(char *));
    if (!d->arena || !d->runs || !d->hrefs || !d->imgs) { html_free(d); return 0; }
    return d;
}

/* a tag that never has a matching close and so must not push a format frame */
static int is_void_tag(const char *name) {
    return strcmp(name,"br")==0 || strcmp(name,"hr")==0 || strcmp(name,"img")==0 ||
           strcmp(name,"meta")==0 || strcmp(name,"link")==0 || strcmp(name,"input")==0 ||
           strcmp(name,"area")==0 || strcmp(name,"base")==0 || strcmp(name,"col")==0 ||
           strcmp(name,"wbr")==0 || strcmp(name,"source")==0 || strcmp(name,"embed")==0;
}

html_doc *html_parse(const char *src, uint32_t len) {
    html_doc *d = doc_alloc(len);
    if (!d) return 0;

    char    seg[SEG_MAX]; uint32_t seglen = 0;
    char    title[TITLE_MAX]; uint32_t titlelen = 0;
    uint8_t style = HSTYLE_NORMAL;
    int     bold = 0, italic = 0, mono = 0, heading = 0, pre = 0, link = -1;
    int     pending_brk = 0, space_pending = 0, line_started = 0;
    int     skip = 0, in_title = 0;
    int     list_depth = 0, bq_depth = 0;

    /* format stack: each open colour/align tag pushes a frame popped on close */
    struct { char name[12]; uint32_t color; uint8_t align; } fst[FSTACK_MAX];
    int fdepth = 0;

    #define CUR_COLOR  (fdepth ? fst[fdepth-1].color : HCOL_NONE)
    #define CUR_ALIGN  (fdepth ? fst[fdepth-1].align : HALIGN_LEFT)
    #define CUR_INDENT ((uint8_t)((list_depth + bq_depth) > 12 ? 12 : (list_depth + bq_depth)))

    #define EFF_STYLE() (pre ? HSTYLE_PRE : heading == 1 ? HSTYLE_H1 : heading == 2 ? HSTYLE_H2 \
                        : heading >= 3 ? HSTYLE_H3 : link >= 0 ? HSTYLE_LINK : mono ? HSTYLE_CODE \
                        : italic ? HSTYLE_ITALIC : bold ? HSTYLE_BOLD : HSTYLE_NORMAL)

    #define FLUSH() do { if (seglen) { push_full(d, seg, seglen, style, link, (uint8_t)pending_brk, \
                                CUR_COLOR, CUR_ALIGN, CUR_INDENT); seglen = 0; pending_brk = 0; } } while (0)

    for (uint32_t i = 0; i < len; ) {
        char c = src[i];

        if (c == '<') {
            if (i + 3 < len && src[i+1] == '!' && src[i+2] == '-' && src[i+3] == '-') {
                i += 4;
                while (i + 2 < len && !(src[i] == '-' && src[i+1] == '-' && src[i+2] == '>')) i++;
                i = (i + 3 < len) ? i + 3 : len;
                continue;
            }
            char tag[512]; uint32_t tl = 0;
            i++;
            while (i < len && src[i] != '>') { if (tl < sizeof(tag) - 1) tag[tl++] = src[i]; i++; }
            tag[tl] = 0;
            if (i < len) i++;

            const char *t = tag;
            int closing = 0;
            while (*t == ' ' || *t == '\t') t++;
            if (*t == '/') { closing = 1; t++; }
            char name[16]; uint32_t nl = 0;
            while (*t && ((lc(*t) >= 'a' && lc(*t) <= 'z') || (*t >= '0' && *t <= '9')) && nl < sizeof(name)-1)
                name[nl++] = lc(*t++);
            name[nl] = 0;

            if (skip) {
                if (closing && (strcmp(name, "script") == 0 || strcmp(name, "style") == 0)) skip = 0;
                continue;
            }

            /* format stack: push on opening colour/align tags, pop on close */
            if (!closing && !is_void_tag(name)) {
                uint32_t col = tag_color(tag);
                uint8_t  al  = tag_align(tag);
                int is_fmt = (col != HCOL_NONE) || (al != HALIGN_LEFT) ||
                             strcmp(name,"center")==0 || strcmp(name,"font")==0 || strcmp(name,"span")==0;
                if (is_fmt && fdepth < FSTACK_MAX) {
                    uint32_t pc = (col != HCOL_NONE) ? col : CUR_COLOR;
                    uint8_t  pa = (al != HALIGN_LEFT) ? al : (strcmp(name,"center")==0 ? HALIGN_CENTER : CUR_ALIGN);
                    uint32_t k = 0; while (name[k] && k < 11) { fst[fdepth].name[k] = name[k]; k++; } fst[fdepth].name[k] = 0;
                    fst[fdepth].color = pc; fst[fdepth].align = pa; fdepth++;
                }
            } else if (closing && fdepth > 0) {
                if (strcmp(fst[fdepth-1].name, name) == 0) fdepth--;
                else for (int s = fdepth - 1; s >= 0; s--) if (strcmp(fst[s].name, name) == 0) { fdepth = s; break; }
            }

            if (strcmp(name, "script") == 0 || strcmp(name, "style") == 0) { if (!closing) skip = 1; continue; }
            if (strcmp(name, "title") == 0) { FLUSH(); in_title = !closing; if (closing && titlelen) {
                    title[titlelen] = 0; d->title = arena_push(d, title, titlelen); titlelen = 0; } continue; }

            if (strcmp(name, "b") == 0 || strcmp(name, "strong") == 0) { FLUSH(); bold = !closing; style = EFF_STYLE(); continue; }
            if (strcmp(name, "i") == 0 || strcmp(name, "em") == 0 || strcmp(name, "cite") == 0 ||
                strcmp(name, "var") == 0 || strcmp(name, "dfn") == 0) { FLUSH(); italic = !closing; style = EFF_STYLE(); continue; }
            if (strcmp(name, "code") == 0 || strcmp(name, "kbd") == 0 || strcmp(name, "samp") == 0 ||
                strcmp(name, "tt") == 0) { FLUSH(); mono = !closing; style = EFF_STYLE(); continue; }
            if (strcmp(name, "a") == 0) {
                FLUSH();
                if (!closing) {
                    char href[256]; get_attr(tag, "href", href, sizeof(href));
                    if (href[0] && d->nhrefs < d->hrefs_cap) {
                        char *h = arena_push(d, href, (uint32_t)strlen(href));
                        if (h) { d->hrefs[d->nhrefs] = h; link = d->nhrefs; d->nhrefs++; }
                    }
                } else link = -1;
                style = EFF_STYLE();
                continue;
            }
            if (strcmp(name, "img") == 0) {
                FLUSH();
                char srcv[256]; get_attr(tag, "src", srcv, sizeof(srcv));
                if (srcv[0] && d->nimgs < d->imgs_cap && d->nruns < d->runs_cap) {
                    char *s = arena_push(d, srcv, (uint32_t)strlen(srcv));
                    if (s) {
                        char wv[12], hv[12];
                        int iw = get_attr(tag, "width", wv, sizeof(wv)) ? atoi(wv) : 0;
                        int ih = get_attr(tag, "height", hv, sizeof(hv)) ? atoi(hv) : 0;
                        char alt[64]; alt[0] = 0; get_attr(tag, "alt", alt, sizeof(alt));
                        const char *lbl = alt[0] ? alt : "[image]";
                        char *at = arena_push(d, lbl, (uint32_t)strlen(lbl));
                        d->imgs[d->nimgs] = s;
                        html_run *r = &d->runs[d->nruns++];
                        r->text = at ? at : s; r->style = HSTYLE_NORMAL; r->link = link;
                        r->brk = (uint8_t)pending_brk; r->kind = HRUN_IMG; r->align = CUR_ALIGN;
                        r->indent = CUR_INDENT; r->color = CUR_COLOR; r->img = d->nimgs;
                        r->iw = iw; r->ih = ih; r->pix = 0;
                        d->nimgs++; pending_brk = 0;
                    }
                }
                continue;
            }
            if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && name[2] == 0) {
                FLUSH(); heading = closing ? 0 : (name[1] - '0'); style = EFF_STYLE();
                if (pending_brk < 2) pending_brk = 2; continue;
            }
            if (strcmp(name, "pre") == 0) { FLUSH(); pre = !closing; style = EFF_STYLE(); if (pending_brk < 2) pending_brk = 2; continue; }
            if (strcmp(name, "br") == 0)  { FLUSH(); if (pending_brk < 1) pending_brk = 1; continue; }
            if (strcmp(name, "ul") == 0 || strcmp(name, "ol") == 0) {
                FLUSH(); if (closing) { if (list_depth) list_depth--; } else list_depth++;
                if (pending_brk < 1) pending_brk = 1; continue;
            }
            if (strcmp(name, "blockquote") == 0) {
                FLUSH(); if (closing) { if (bq_depth) bq_depth--; } else bq_depth++;
                if (pending_brk < 2) pending_brk = 2; continue;
            }
            if (strcmp(name, "li") == 0 && !closing) {
                FLUSH(); pending_brk = pending_brk < 1 ? 1 : pending_brk;
                push_full(d, "- ", 2, HSTYLE_NORMAL, -1, (uint8_t)pending_brk, CUR_COLOR, CUR_ALIGN, CUR_INDENT);
                pending_brk = 0; continue;
            }
            if (strcmp(name, "hr") == 0) { FLUSH(); pending_brk = 2;
                push_full(d, "--------------------------------", 32, HSTYLE_NORMAL, -1, 2, CUR_COLOR, HALIGN_LEFT, CUR_INDENT);
                pending_brk = 1; continue; }
            if (strcmp(name, "p") == 0 || strcmp(name, "div") == 0 ||
                strcmp(name, "table") == 0 || strcmp(name, "tr") == 0 ||
                strcmp(name, "section") == 0 || strcmp(name, "article") == 0 || strcmp(name, "header") == 0 ||
                strcmp(name, "footer") == 0 || strcmp(name, "nav") == 0 ||
                strcmp(name, "center") == 0 || strcmp(name, "figure") == 0 || strcmp(name, "figcaption") == 0) {
                FLUSH(); int w = (strcmp(name, "p") == 0) ? 2 : 1;
                if (pending_brk < w) pending_brk = w;
                continue;
            }
            if ((strcmp(name, "td") == 0 || strcmp(name, "th") == 0) && !closing) {
                FLUSH(); /* a couple of spaces to separate table cells on a line */
                push_full(d, "  ", 2, HSTYLE_NORMAL, -1, 0, CUR_COLOR, CUR_ALIGN, CUR_INDENT);
                continue;
            }
            continue;   /* unknown / inline tag: ignored, text flows through */
        }

        if (skip) { i++; continue; }

        if (c == '&') {
            int e = decode_entity(src, &i, len);
            if (e >= 0) c = (char)e; else { i++; }
        } else if ((unsigned char)c >= 0x80) {              /* UTF-8 multibyte */
            uint32_t cp; int adv = utf8_decode((const unsigned char *)(src + i), len - i, &cp);
            i += adv > 0 ? (uint32_t)adv : 1;
            int m = fold_cp(cp);
            if (m < 0) continue;                            /* unrenderable: drop */
            c = (char)m;
        } else i++;

        if (in_title) {
            if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
                if (titlelen && title[titlelen-1] != ' ' && titlelen < TITLE_MAX-1) title[titlelen++] = ' ';
            } else if (titlelen < TITLE_MAX - 1) title[titlelen++] = c;
            continue;
        }

        if (pre) {
            if (c == '\n') { FLUSH(); pending_brk = 1; }
            else if (c == '\r') { }
            else if (seglen < SEG_MAX - 1) seg[seglen++] = c;
            continue;
        }

        if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
            space_pending = 1;
        } else {
            if (space_pending && seglen < SEG_MAX - 1) {
                if (seglen > 0)                            seg[seglen++] = ' ';
                else if (pending_brk == 0 && line_started) seg[seglen++] = ' ';
            }
            space_pending = 0;
            if (seglen >= SEG_MAX - 1) { push_full(d, seg, seglen, style, link, (uint8_t)pending_brk, CUR_COLOR, CUR_ALIGN, CUR_INDENT); seglen = 0; pending_brk = 0; }
            seg[seglen++] = c;
            line_started = 1;
        }
    }
    if (seglen) push_full(d, seg, seglen, style, link, (uint8_t)pending_brk, CUR_COLOR, CUR_ALIGN, CUR_INDENT);
    return d;
}

html_doc *html_parse_text(const char *src, uint32_t len) {
    html_doc *d = doc_alloc(len);
    if (!d) return 0;
    char seg[SEG_MAX]; uint32_t seglen = 0; uint8_t brk = 0;
    for (uint32_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == '\r') continue;
        if (c == '\n') {
            push_full(d, seglen ? seg : " ", seglen ? seglen : 1, HSTYLE_PRE, -1, brk, HCOL_NONE, HALIGN_LEFT, 0);
            seglen = 0; brk = 1; continue;
        }
        if (seglen < SEG_MAX - 1) seg[seglen++] = c;
    }
    if (seglen) push_full(d, seg, seglen, HSTYLE_PRE, -1, brk, HCOL_NONE, HALIGN_LEFT, 0);
    return d;
}

void html_free(html_doc *d) {
    if (!d) return;
    if (d->arena) kfree(d->arena);
    if (d->runs)  kfree(d->runs);
    if (d->hrefs) kfree(d->hrefs);
    if (d->imgs)  kfree(d->imgs);
    kfree(d);
}
