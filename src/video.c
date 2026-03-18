/*
 * video.c — video clip loader: JPEG directory and MJPEG AVI.
 */

#include "video.h"

#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static int is_jpeg(const char *name) {
    const char *ext = strrchr(name, '.');
    if (!ext) return 0;
    return strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0;
}

/* Read a whole file into a new malloc buffer. Caller frees. */
static uint8_t *read_file(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    fseek(f, 0, SEEK_END);
    size_t sz = (size_t)ftell(f);
    rewind(f);
    uint8_t *buf = malloc(sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, sz, f) != sz) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_size = sz;
    return buf;
}

/* ------------------------------------------------------------------ */
/* video_load  (JPEG directory)                                        */
/* ------------------------------------------------------------------ */

int video_load(const char *dir, VideoClip *vc) {
    memset(vc, 0, sizeof(*vc));
    vc->fd = -1;
    strncpy(vc->path, dir, sizeof(vc->path) - 1);

    const char *slash = strrchr(dir, '/');
    strncpy(vc->name, slash ? slash + 1 : dir, sizeof(vc->name) - 1);

    struct dirent **entries;
    int n = scandir(dir, &entries, NULL, alphasort);
    if (n < 0) { perror(dir); return 0; }

    int count = 0;
    for (int i = 0; i < n; i++)
        if (is_jpeg(entries[i]->d_name)) count++;

    if (count == 0) {
        fprintf(stderr, "video: no JPEGs in %s\n", dir);
        for (int i = 0; i < n; i++) free(entries[i]);
        free(entries);
        return 0;
    }
    if (count > VIDEO_MAX_FRAMES) count = VIDEO_MAX_FRAMES;

    vc->offsets = malloc(count * sizeof(size_t));
    vc->sizes   = malloc(count * sizeof(size_t));
    if (!vc->offsets || !vc->sizes) {
        free(vc->offsets); free(vc->sizes);
        for (int i = 0; i < n; i++) free(entries[i]);
        free(entries);
        return 0;
    }

    /* First pass: measure total compressed size */
    size_t total_bytes = 0;
    {
        int fi = 0;
        for (int i = 0; i < n && fi < count; i++) {
            if (!is_jpeg(entries[i]->d_name)) continue;
            char path[768];
            snprintf(path, sizeof(path), "%s/%s", dir, entries[i]->d_name);
            FILE *f = fopen(path, "rb");
            if (!f) { fi++; continue; }
            fseek(f, 0, SEEK_END);
            size_t sz = (size_t)ftell(f);
            vc->sizes[fi++] = sz;
            total_bytes += sz;
            fclose(f);
        }
        vc->num_frames = fi;
    }

    vc->source = malloc(total_bytes);
    if (!vc->source) {
        fprintf(stderr, "video: out of memory (%zuMB) for %s\n",
                total_bytes / (1024*1024), dir);
        free(vc->offsets); free(vc->sizes);
        for (int i = 0; i < n; i++) free(entries[i]);
        free(entries);
        return 0;
    }
    vc->source_size = total_bytes;

    /* Second pass: load JPEG bytes */
    size_t cursor = 0;
    int fi = 0;
    for (int i = 0; i < n && fi < vc->num_frames; i++) {
        if (!is_jpeg(entries[i]->d_name)) continue;
        char path[768];
        snprintf(path, sizeof(path), "%s/%s", dir, entries[i]->d_name);
        size_t sz;
        uint8_t *tmp = read_file(path, &sz);
        if (tmp) {
            vc->offsets[fi] = cursor;
            memcpy(vc->source + cursor, tmp, sz);
            free(tmp);
            cursor += sz;
        }
        fi++;
    }
    for (int i = 0; i < n; i++) free(entries[i]);
    free(entries);

    /* Width/height from first frame */
    tjhandle tj = tjInitDecompress();
    if (!tj) {
        fprintf(stderr, "video: tjInitDecompress failed\n");
        video_unload(vc);
        return 0;
    }
    int w, h, subsamp, colorspace;
    if (tjDecompressHeader3(tj, vc->source, vc->sizes[0],
                             &w, &h, &subsamp, &colorspace) < 0) {
        fprintf(stderr, "video: bad first frame in %s: %s\n", dir, tjGetErrorStr());
        tjDestroy(tj);
        video_unload(vc);
        return 0;
    }
    tjDestroy(tj);
    vc->width  = w;
    vc->height = h;
    vc->fps    = 30.0f;

    vc->pixels = malloc((size_t)w * (size_t)h * 4);
    if (!vc->pixels) {
        fprintf(stderr, "video: out of memory for decode buffer\n");
        video_unload(vc);
        return 0;
    }

    printf("  video[?] %s  %d frames  %dx%d  %.1fMB\n",
           vc->name, vc->num_frames, w, h,
           (float)total_bytes / (1024.0f * 1024.0f));
    return 1;
}

/* ------------------------------------------------------------------ */
/* RIFF/AVI parser helpers                                             */
/* ------------------------------------------------------------------ */

/* All AVI structs are little-endian. These helpers read from a raw
 * byte pointer without alignment assumptions. */

static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)(p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24));
}

/* FourCC comparison */
static int fcc(const uint8_t *p, const char *tag) {
    return p[0]==tag[0] && p[1]==tag[1] && p[2]==tag[2] && p[3]==tag[3];
}

/*
 * RIFF chunk layout:
 *   [4] FourCC
 *   [4] size  (LE, bytes of data, NOT including this 8-byte header)
 *   [size] data  (padded to even byte boundary)
 *
 * LIST chunk:
 *   "LIST" [4] size [4] listType [size-4] children
 */

/* Scan the movi LIST for "00dc" (stream 0 compressed video) chunks.
 * movi_data points to the first byte AFTER the "movi" fourCC tag
 * (i.e. the first child chunk). movi_size is the size from the LIST
 * header minus 4 (for the "movi" tag itself).
 * Fills vc->offsets/sizes; base_offset is the byte offset of
 * movi_data[0] from vc->source[0]. */
static int scan_movi(VideoClip *vc, const uint8_t *movi_data, size_t movi_size,
                     size_t base_offset) {
    size_t pos = 0;
    int fi = 0;

    while (pos + 8 <= movi_size && fi < VIDEO_MAX_FRAMES) {
        const uint8_t *hdr = movi_data + pos;
        uint32_t chunk_size = rd32(hdr + 4);

        /* Accept "00dc" and "00db" (compressed/uncompressed video stream 0) */
        int is_video = (hdr[0]=='0' && hdr[1]=='0' &&
                        (hdr[2]=='d' || hdr[2]=='D') &&
                        (hdr[3]=='c' || hdr[3]=='C' ||
                         hdr[3]=='b' || hdr[3]=='B'));

        if (is_video && chunk_size > 0) {
            vc->offsets[fi] = base_offset + pos + 8;
            vc->sizes[fi]   = chunk_size;
            fi++;
        }

        pos += 8 + chunk_size;
        if (chunk_size & 1) pos++; /* RIFF word-alignment padding */
    }
    vc->num_frames = fi;
    return fi > 0 ? 1 : 0;
}

/* Use idx1 index to fill offsets/sizes.
 * idx1_data: bytes after "idx1" FourCC+size header.
 * idx1_size: chunk data size.
 * movi_offset: absolute offset (from vc->source) of the first byte of
 *              the movi LIST chunk header (the "LIST" fourCC). */
static int parse_idx1(VideoClip *vc, const uint8_t *idx1_data, uint32_t idx1_size,
                      size_t movi_offset) {
    /*
     * idx1 entry: [4] FourCC [4] flags [4] offset [4] size
     * offset is either absolute (from file start) or relative to movi data
     * start (after "movi" tag).  Heuristic: if first entry's offset > movi_offset
     * it's absolute; otherwise relative to (movi_offset + 4).
     */
    if (idx1_size < 16) return 0;

    uint32_t first_off = rd32(idx1_data + 8);
    int absolute = (first_off > (uint32_t)movi_offset);
    /* movi data starts 12 bytes into the LIST: "LIST"(4) + size(4) + "movi"(4) */
    size_t movi_data_base = movi_offset + 12;

    int fi = 0;
    for (uint32_t pos = 0; pos + 16 <= idx1_size && fi < VIDEO_MAX_FRAMES; pos += 16) {
        const uint8_t *e = idx1_data + pos;
        uint32_t eoff  = rd32(e + 8);
        uint32_t esz   = rd32(e + 12);

        int is_video = (e[0]=='0' && e[1]=='0' &&
                        (e[2]=='d' || e[2]=='D') &&
                        (e[3]=='c' || e[3]=='C' ||
                         e[3]=='b' || e[3]=='B'));
        if (!is_video || esz == 0) continue;

        size_t abs_offset;
        if (absolute)
            abs_offset = (size_t)eoff + 8; /* skip chunk header at that pos */
        else
            abs_offset = movi_data_base + (size_t)eoff + 8;

        if (abs_offset + esz > vc->source_size) continue; /* sanity check */

        vc->offsets[fi] = abs_offset;
        vc->sizes[fi]   = esz;
        fi++;
    }
    vc->num_frames = fi;
    return fi > 0 ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* video_load_avi  (MJPEG AVI, mmap'd)                                */
/* ------------------------------------------------------------------ */

int video_load_avi(const char *file, VideoClip *vc) {
    memset(vc, 0, sizeof(*vc));
    vc->fd = -1;

    strncpy(vc->path, file, sizeof(vc->path) - 1);
    const char *slash = strrchr(file, '/');
    const char *base  = slash ? slash + 1 : file;
    /* Strip extension for name */
    const char *dot   = strrchr(base, '.');
    size_t nlen = dot ? (size_t)(dot - base) : strlen(base);
    if (nlen >= sizeof(vc->name)) nlen = sizeof(vc->name) - 1;
    memcpy(vc->name, base, nlen);
    vc->name[nlen] = '\0';

    /* Open and mmap the file */
    int fd = open(file, O_RDONLY);
    if (fd < 0) { perror(file); return 0; }

    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 12) {
        fprintf(stderr, "video: bad AVI file %s\n", file);
        close(fd);
        return 0;
    }
    size_t fsize = (size_t)st.st_size;

    uint8_t *data = mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return 0;
    }

    /* Validate RIFF....AVI  header */
    if (!fcc(data, "RIFF") || !fcc(data + 8, "AVI ")) {
        fprintf(stderr, "video: %s is not an AVI file\n", file);
        munmap(data, fsize);
        close(fd);
        return 0;
    }

    vc->source      = data;
    vc->source_size = fsize;
    vc->fd          = fd;
    vc->fps         = 30.0f; /* fallback */

    /* Allocate frame index arrays (upper bound) */
    vc->offsets = malloc(VIDEO_MAX_FRAMES * sizeof(size_t));
    vc->sizes   = malloc(VIDEO_MAX_FRAMES * sizeof(size_t));
    if (!vc->offsets || !vc->sizes) {
        video_unload(vc);
        return 0;
    }

    /* ---- Walk top-level RIFF chunks ---- */
    size_t pos = 12; /* skip "RIFF" + size + "AVI " */
    size_t movi_offset = 0;   /* absolute offset of movi LIST header */
    size_t movi_data   = 0;   /* absolute offset of first byte after "movi" tag */
    size_t movi_size   = 0;   /* LIST data size - 4 */
    const uint8_t *idx1_ptr  = NULL;
    uint32_t       idx1_size = 0;

    while (pos + 8 <= fsize) {
        const uint8_t *hdr = data + pos;
        uint32_t csz = rd32(hdr + 4);

        if (fcc(hdr, "LIST")) {
            if (pos + 12 <= fsize) {
                if (fcc(hdr + 8, "hdrl")) {
                    /* Header LIST: walk for avih and strl */
                    size_t hpos = pos + 12;
                    size_t hend = pos + 8 + csz;
                    if (hend > fsize) hend = fsize;
                    while (hpos + 8 <= hend) {
                        const uint8_t *h2 = data + hpos;
                        uint32_t h2sz = rd32(h2 + 4);
                        if (fcc(h2, "avih") && h2sz >= 40) {
                            /* AVIMAINHEADER: dwMicroSecPerFrame at offset 8 */
                            uint32_t usec = rd32(h2 + 8 + 0);
                            if (usec > 0)
                                vc->fps = 1000000.0f / (float)usec;
                        }
                        hpos += 8 + h2sz + (h2sz & 1);
                    }
                } else if (fcc(hdr + 8, "movi")) {
                    movi_offset = pos;
                    movi_data   = pos + 12;
                    movi_size   = csz >= 4 ? csz - 4 : 0;
                }
            }
        } else if (fcc(hdr, "idx1") && csz > 0) {
            if (pos + 8 + csz <= fsize) {
                idx1_ptr  = data + pos + 8;
                idx1_size = csz;
            }
        }

        pos += 8 + csz + (csz & 1);
    }

    if (movi_offset == 0) {
        fprintf(stderr, "video: no movi chunk in %s\n", file);
        video_unload(vc);
        return 0;
    }

    /* Build frame index */
    int ok;
    if (idx1_ptr)
        ok = parse_idx1(vc, idx1_ptr, idx1_size, movi_offset);
    else
        ok = scan_movi(vc, data + movi_data, movi_size, movi_data);

    if (!ok || vc->num_frames == 0) {
        fprintf(stderr, "video: no video frames found in %s\n", file);
        video_unload(vc);
        return 0;
    }

    /* Determine w/h from first JPEG frame */
    tjhandle tj = tjInitDecompress();
    if (!tj) {
        fprintf(stderr, "video: tjInitDecompress failed\n");
        video_unload(vc);
        return 0;
    }
    int w, h, subsamp, colorspace;
    if (tjDecompressHeader3(tj,
            vc->source + vc->offsets[0], (unsigned long)vc->sizes[0],
            &w, &h, &subsamp, &colorspace) < 0) {
        fprintf(stderr, "video: bad first frame in %s: %s\n", file, tjGetErrorStr());
        tjDestroy(tj);
        video_unload(vc);
        return 0;
    }
    tjDestroy(tj);
    vc->width  = w;
    vc->height = h;

    vc->pixels = malloc((size_t)w * (size_t)h * 4);
    if (!vc->pixels) {
        fprintf(stderr, "video: out of memory for decode buffer\n");
        video_unload(vc);
        return 0;
    }

    printf("  video[?] %s  %d frames  %dx%d  %.1ffps  %.1fMB (mmap)\n",
           vc->name, vc->num_frames, w, h, vc->fps,
           (float)fsize / (1024.0f * 1024.0f));
    return 1;
}

/* ------------------------------------------------------------------ */
/* video_decode_frame                                                  */
/* ------------------------------------------------------------------ */

int video_decode_frame(VideoClip *vc, int idx, tjhandle tj) {
    if (idx < 0 || idx >= vc->num_frames) return -1;
    const uint8_t *src  = vc->source + vc->offsets[idx];
    size_t         size = vc->sizes[idx];
    int ret = tjDecompress2(tj, src, (unsigned long)size,
                             vc->pixels, vc->width, 0, vc->height,
                             TJPF_RGBA, TJFLAG_FASTDCT);
    if (ret < 0) {
        fprintf(stderr, "video: decode error frame %d: %s\n", idx, tjGetErrorStr());
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* video_unload                                                        */
/* ------------------------------------------------------------------ */

void video_unload(VideoClip *vc) {
    if (vc->fd >= 0) {
        if (vc->source) munmap(vc->source, vc->source_size);
        close(vc->fd);
    } else {
        free(vc->source);
    }
    free(vc->offsets);
    free(vc->sizes);
    free(vc->pixels);
    memset(vc, 0, sizeof(*vc));
    vc->fd = -1;
}
