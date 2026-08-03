// Mod made by Realbiquitous
/*
 * saveengine.h - Core Cyberpunk 2077 save file read/write engine.
 *
 * Format reverse-engineered from:
 *   - rfuzzo/red4lib (archive format reference, unrelated but same author's rigor)
 *   - PixelRick/CyberpunkSaveEditor (C++ source - the primary reference for
 *     this exact format: header.rs, node_tree.cpp, serial_tree.h, cobject.h,
 *     cproperty.h, cproperty_factory.cpp)
 *   - Deweh/CyberCAT-SimpleGUI + WolvenKit (C# source - confirmed
 *     PlayerDevelopmentData/SProficiency field names)
 *
 * Verified against a real save file: Level and StreetCred both matched the
 * game's own metadata.json exactly, and a full read-modify-write round trip
 * was independently re-verified by re-parsing the written output.
 */

#ifndef SAVEENGINE_H
#define SAVEENGINE_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <lz4.h>

#define MAGIC_CSAV 0x43534156u
#define MAGIC_SAVE 0x45564153u
#define MAGIC_DONE 0x444F4E45u
#define MAGIC_NODE 0x4E4F4445u
#define MAGIC_CLZF 0x434C5A46u
#define MAGIC_XLZ4 0x584C5A34u

#define MAX_PROFICIENCIES 40

typedef struct {
    int32_t next_idx, child_idx;
    uint32_t data_offset, data_size;
    char name[256];
} SE_NodeDesc;

typedef struct {
    uint32_t offset, size, data_size, data_offset_calc;
    int dirty; /* set true if this chunk's decompressed content was modified */
} SE_ChunkDesc;

typedef struct {
    char name[64];        /* proficiency type name, e.g. "Level" */
    int32_t currentValue; /* original value read */
    int32_t newValue;     /* editable - starts equal to currentValue */
    uint32_t absPos;      /* absolute position within nodedata of the currentLevel field */
} SE_Proficiency;

typedef struct {
    unsigned char *rawFile;
    long fileSize;

    uint32_t v1;
    long chunkTableEntriesStart; /* file position right after chunk count */
    long chunkTableEnd;
    long footerStart;
    uint32_t nodedescsStart;

    SE_NodeDesc *descs;
    int64_t nodeCount;

    SE_ChunkDesc *chunks;
    uint32_t chunkCount;

    char *nodedata;
    uint64_t nodedataSize;

    SE_Proficiency proficiencies[MAX_PROFICIENCIES];
    int proficiencyCount;

    char lastError[512];
} SaveFile;

/* ---- low level readers ---- */

static int64_t se_read_int64_packed(FILE *f) {
    uint8_t a;
    if (fread(&a, 1, 1, f) != 1) return 0;
    int64_t value = a & 0x3F;
    int sign = (a & 0x80) != 0;
    if (a & 0x40) {
        if (fread(&a, 1, 1, f) != 1) return 0;
        value |= ((int64_t)(a & 0x7F)) << 6;
        if (a & 0x80) {
            if (fread(&a, 1, 1, f) != 1) return 0;
            value |= ((int64_t)(a & 0x7F)) << 13;
            if (a & 0x80) {
                if (fread(&a, 1, 1, f) != 1) return 0;
                value |= ((int64_t)(a & 0x7F)) << 20;
                if (a & 0x80) {
                    if (fread(&a, 1, 1, f) != 1) return 0;
                    value |= ((int64_t)(a & 0xFF)) << 27;
                }
            }
        }
    }
    return sign ? -value : value;
}

static void se_read_str_lpfxd(FILE *f, char *out, size_t outSize) {
    int64_t cnt = se_read_int64_packed(f);
    out[0] = '\0';
    if (cnt < 0 && cnt > -0x1000) {
        size_t len = (size_t)(-cnt);
        size_t toRead = len < outSize - 1 ? len : outSize - 1;
        if (fread(out, 1, toRead, f) != toRead) { /* best effort */ }
        out[toRead] = '\0';
        if (toRead < len) fseek(f, (long)(len - toRead), SEEK_CUR);
    } else if (cnt > 0 && cnt < 0x1000) {
        size_t len = (size_t)cnt;
        for (size_t i = 0; i < len; i++) {
            uint16_t wc = 0;
            if (fread(&wc, 2, 1, f) != 1) { /* best effort */ }
            if (i < outSize - 1) out[i] = (char)(wc & 0xFF);
        }
        out[len < outSize - 1 ? len : outSize - 1] = '\0';
    }
}

static uint32_t se_read_u32(FILE *f) {
    uint32_t v = 0;
    if (fread(&v, 4, 1, f) != 1) { /* best effort */ }
    return v;
}

/* ---- string pool ---- */

typedef struct {
    char **strings;
    int count;
} SE_StringPool;

static SE_StringPool se_read_string_pool(const char *buffer, uint32_t descsSize) {
    SE_StringPool pool;
    pool.count = (int)(descsSize / 4);
    pool.strings = (char **)malloc((size_t)pool.count * sizeof(char *));
    for (int i = 0; i < pool.count; i++) {
        uint32_t raw = *(uint32_t *)(buffer + i * 4);
        uint32_t offset = raw & 0x00FFFFFF;
        uint8_t sz = (uint8_t)((raw >> 24) & 0xFF);
        int strLen = sz > 0 ? sz - 1 : 0;
        char *s = (char *)malloc((size_t)strLen + 1);
        memcpy(s, buffer + offset, (size_t)strLen);
        s[strLen] = '\0';
        pool.strings[i] = s;
    }
    return pool;
}

static const char *se_pool_get(SE_StringPool *pool, uint32_t idx) {
    if ((int)idx >= pool->count) return "<invalid>";
    return pool->strings[idx];
}

static void se_free_pool(SE_StringPool *pool) {
    for (int i = 0; i < pool->count; i++) free(pool->strings[i]);
    free(pool->strings);
}

/* ---- main load function ---- */

int SaveFile_Load(SaveFile *sf, const char *path) {
    memset(sf, 0, sizeof(SaveFile));

    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(sf->lastError, sizeof(sf->lastError), "Could not open file"); return 0; }

    fseek(f, 0, SEEK_END);
    sf->fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    sf->rawFile = (unsigned char *)malloc((size_t)sf->fileSize);
    if (fread(sf->rawFile, 1, (size_t)sf->fileSize, f) != (size_t)sf->fileSize) {
        snprintf(sf->lastError, sizeof(sf->lastError), "Short read on file");
        fclose(f); return 0;
    }
    fseek(f, 0, SEEK_SET);

    uint32_t magic = se_read_u32(f);
    if (magic != MAGIC_CSAV && magic != MAGIC_SAVE) {
        snprintf(sf->lastError, sizeof(sf->lastError), "Not a valid Cyberpunk save file (bad magic)");
        fclose(f); return 0;
    }

    sf->v1 = se_read_u32(f);
    se_read_u32(f); /* v2 */
    char suk[256];
    se_read_str_lpfxd(f, suk, sizeof(suk));
    se_read_u32(f); se_read_u32(f); /* uk0, uk1 */
    if (sf->v1 >= 83) se_read_u32(f);

    long chunkdescsStart = ftell(f);

    fseek(f, -8, SEEK_END);
    sf->footerStart = ftell(f);
    sf->nodedescsStart = se_read_u32(f);
    uint32_t doneMagic = se_read_u32(f);
    if (doneMagic != MAGIC_DONE) {
        snprintf(sf->lastError, sizeof(sf->lastError), "Missing DONE footer tag - file may be corrupt");
        fclose(f); return 0;
    }

    fseek(f, sf->nodedescsStart, SEEK_SET);
    uint32_t nodeMagic = se_read_u32(f);
    if (nodeMagic != MAGIC_NODE) {
        snprintf(sf->lastError, sizeof(sf->lastError), "Missing NODE tag - file may be corrupt");
        fclose(f); return 0;
    }
    sf->nodeCount = se_read_int64_packed(f);
    if (sf->nodeCount <= 0 || sf->nodeCount > 100000) {
        snprintf(sf->lastError, sizeof(sf->lastError), "Implausible node count");
        fclose(f); return 0;
    }

    sf->descs = (SE_NodeDesc *)malloc((size_t)sf->nodeCount * sizeof(SE_NodeDesc));
    for (int64_t i = 0; i < sf->nodeCount; i++) {
        se_read_str_lpfxd(f, sf->descs[i].name, sizeof(sf->descs[i].name));
        sf->descs[i].next_idx = (int32_t)se_read_u32(f);
        sf->descs[i].child_idx = (int32_t)se_read_u32(f);
        sf->descs[i].data_offset = se_read_u32(f);
        sf->descs[i].data_size = se_read_u32(f);
    }

    fseek(f, chunkdescsStart, SEEK_SET);
    uint32_t clzfMagic = se_read_u32(f);
    if (clzfMagic != MAGIC_CLZF) {
        snprintf(sf->lastError, sizeof(sf->lastError), "Missing CLZF tag - file may be corrupt");
        fclose(f); return 0;
    }
    sf->chunkCount = se_read_u32(f);
    sf->chunkTableEntriesStart = ftell(f);

    sf->chunks = (SE_ChunkDesc *)calloc(sf->chunkCount, sizeof(SE_ChunkDesc));
    for (uint32_t i = 0; i < sf->chunkCount; i++) {
        sf->chunks[i].offset = se_read_u32(f);
        sf->chunks[i].size = se_read_u32(f);
        sf->chunks[i].data_size = se_read_u32(f);
    }
    sf->chunkTableEnd = ftell(f);

    /* sorted view by file offset */
    uint32_t *sortedIdx = (uint32_t *)malloc(sf->chunkCount * sizeof(uint32_t));
    for (uint32_t i = 0; i < sf->chunkCount; i++) sortedIdx[i] = i;
    for (uint32_t i = 1; i < sf->chunkCount; i++) {
        uint32_t key = sortedIdx[i];
        int j = (int)i - 1;
        while (j >= 0 && sf->chunks[sortedIdx[j]].offset > sf->chunks[key].offset) {
            sortedIdx[j + 1] = sortedIdx[j];
            j--;
        }
        sortedIdx[j + 1] = key;
    }

    uint32_t runningOffset = sf->chunkCount > 0 ? sf->chunks[sortedIdx[0]].offset : 0;
    for (uint32_t i = 0; i < sf->chunkCount; i++) {
        uint32_t ci = sortedIdx[i];
        sf->chunks[ci].data_offset_calc = runningOffset;
        runningOffset += sf->chunks[ci].data_size;
    }
    sf->nodedataSize = runningOffset;

    sf->nodedata = (char *)calloc(1, sf->nodedataSize);
    char *compBuf = (char *)malloc(sf->nodedataSize);

    for (uint32_t i = 0; i < sf->chunkCount; i++) {
        uint32_t ci = sortedIdx[i];
        fseek(f, sf->chunks[ci].offset, SEEK_SET);
        uint32_t xlz4Magic = se_read_u32(f);
        if (xlz4Magic != MAGIC_XLZ4) {
            snprintf(sf->lastError, sizeof(sf->lastError), "Chunk %u: missing XLZ4 tag", ci);
            free(compBuf); free(sortedIdx); fclose(f); return 0;
        }
        se_read_u32(f);
        size_t csize = sf->chunks[ci].size - 8;
        if (fread(compBuf, 1, csize, f) != csize) { /* best effort */ }
        int res = LZ4_decompress_safe(compBuf, sf->nodedata + sf->chunks[ci].data_offset_calc, (int)csize, (int)sf->chunks[ci].data_size);
        if (res != (int)sf->chunks[ci].data_size) {
            snprintf(sf->lastError, sizeof(sf->lastError), "Chunk %u: decompression failed", ci);
            free(compBuf); free(sortedIdx); fclose(f); return 0;
        }
    }
    free(compBuf);
    free(sortedIdx);
    fclose(f);

    /* --- Locate proficiencies array via ScriptableSystemsContainer --- */
    int64_t sscIdx = -1;
    for (int64_t i = 0; i < sf->nodeCount; i++) {
        if (strcmp(sf->descs[i].name, "ScriptableSystemsContainer") == 0) { sscIdx = i; break; }
    }
    if (sscIdx < 0) {
        snprintf(sf->lastError, sizeof(sf->lastError), "ScriptableSystemsContainer node not found");
        return 0;
    }

    const char *nodeData = sf->nodedata + sf->descs[sscIdx].data_offset;
    uint32_t blob_size = *(uint32_t *)(nodeData + 4);
    uint32_t blob_spos = 8;
    uint32_t cnames_cnt = *(uint32_t *)(nodeData + 12);
    uint32_t pos = 16; /* nodeIdx(4) + blob_size(4) + uk1(2) + uk2(2) + cnames_cnt(4) = 16 */
    uint32_t strpool_data_offset_field2 = *(uint32_t *)(nodeData + pos + 4);
    uint32_t obj_descs_offset = *(uint32_t *)(nodeData + pos + 8);
    uint32_t objdata_offset = *(uint32_t *)(nodeData + pos + 12);
    pos += 16;
    if (cnames_cnt > 1) {
        uint32_t cnc2 = *(uint32_t *)(nodeData + pos); pos += 4;
        pos += cnc2 * 8;
    }
    uint32_t base_offset = pos - blob_spos;
    const char *blob = nodeData + pos;
    (void)base_offset; (void)blob_size;

    SE_StringPool pool = se_read_string_pool(blob, strpool_data_offset_field2);

    const char *objDescsPtr = blob + obj_descs_offset;
    uint32_t objDescsCnt = (objdata_offset - obj_descs_offset) / 8;
    const char *objDataPtr = blob + objdata_offset;

    uint32_t pdsFieldNameIdx = 0;
    for (int i = 0; i < pool.count; i++) if (strcmp(pool.strings[i], "PlayerDevelopmentSystem") == 0) pdsFieldNameIdx = (uint32_t)i;

    uint32_t pdsOffset = 0; int found = 0;
    for (uint32_t i = 0; i < objDescsCnt && !found; i++) {
        uint32_t nameIdx = *(uint32_t *)(objDescsPtr + i * 8);
        if (nameIdx == pdsFieldNameIdx) {
            pdsOffset = *(uint32_t *)(objDescsPtr + i * 8 + 4) - objdata_offset;
            found = 1;
        }
    }
    if (!found) {
        snprintf(sf->lastError, sizeof(sf->lastError), "PlayerDevelopmentSystem not found");
        se_free_pool(&pool);
        return 0;
    }

    uint32_t countPos = pdsOffset + 10;
    uint32_t handleVal = *(uint32_t *)(objDataPtr + countPos + 4);
    uint32_t pddOff = *(uint32_t *)(objDescsPtr + handleVal * 8 + 4) - objdata_offset;

    uint16_t fcnt2 = *(uint16_t *)(objDataPtr + pddOff);
    uint32_t db2 = pddOff + 2;
    uint32_t dataStart3 = db2 + fcnt2 * 8;
    int profFieldIdx = -1;
    uint32_t profOff = 0;
    for (int fld = 0; fld < fcnt2; fld++) {
        uint16_t nidx = *(uint16_t *)(objDataPtr + db2 + fld * 8);
        uint32_t off = *(uint32_t *)(objDataPtr + db2 + fld * 8 + 4);
        if ((int)nidx < pool.count && strcmp(pool.strings[nidx], "proficiencies") == 0) {
            profFieldIdx = fld;
            profOff = dataStart3 + off;
        }
    }
    if (profFieldIdx < 0) {
        snprintf(sf->lastError, sizeof(sf->lastError), "proficiencies field not found");
        se_free_pool(&pool);
        return 0;
    }

    uint32_t entryStart = profOff + 8;
    uint32_t e = entryStart;
    sf->proficiencyCount = 0;
    for (int pi = 0; pi < 30 && sf->proficiencyCount < MAX_PROFICIENCIES; pi++) {
        uint16_t pfcnt = *(uint16_t *)(objDataPtr + e);
        if (pfcnt == 0 || pfcnt > 6) break;
        int32_t typeOff = -1, levelOff = -1, maxOff3 = -1;
        for (int fld = 0; fld < pfcnt; fld++) {
            uint16_t nidx = *(uint16_t *)(objDataPtr + e + 2 + fld * 8);
            uint32_t off = *(uint32_t *)(objDataPtr + e + 2 + fld * 8 + 4);
            const char *fname = ((int)nidx < pool.count) ? pool.strings[nidx] : "?";
            if (strcmp(fname, "type") == 0) typeOff = (int32_t)off;
            if (strcmp(fname, "currentLevel") == 0) levelOff = (int32_t)off;
            if ((int32_t)off > maxOff3) maxOff3 = (int32_t)off;
        }
        if (typeOff >= 0 && levelOff >= 0) {
            uint16_t typeValAsIdx = *(uint16_t *)(objDataPtr + e + typeOff);
            const char *typeStr = ((int)typeValAsIdx < pool.count) ? pool.strings[typeValAsIdx] : "?";
            const char *fieldPtr = objDataPtr + e + levelOff;

            SE_Proficiency *p = &sf->proficiencies[sf->proficiencyCount];
            strncpy(p->name, typeStr, sizeof(p->name) - 1);
            p->currentValue = *(int32_t *)fieldPtr;
            p->newValue = p->currentValue;
            p->absPos = (uint32_t)(fieldPtr - sf->nodedata);
            sf->proficiencyCount++;
        }
        if (maxOff3 < 0) break;
        uint32_t esize = 2 + (uint32_t)pfcnt * 8;
        if ((uint32_t)maxOff3 + 4 > esize) esize = (uint32_t)maxOff3 + 4;
        e += esize;
    }

    se_free_pool(&pool);
    return 1;
}

/* Mark all chunks whose range contains any changed proficiency as dirty */
static void SaveFile_MarkDirtyChunks(SaveFile *sf) {
    for (int p = 0; p < sf->proficiencyCount; p++) {
        if (sf->proficiencies[p].newValue == sf->proficiencies[p].currentValue) continue;
        for (uint32_t c = 0; c < sf->chunkCount; c++) {
            if ((uint64_t)sf->proficiencies[p].absPos >= sf->chunks[c].data_offset_calc &&
                (uint64_t)sf->proficiencies[p].absPos < (uint64_t)sf->chunks[c].data_offset_calc + sf->chunks[c].data_size) {
                sf->chunks[c].dirty = 1;
            }
        }
    }
}

/* Apply all pending edits to nodedata in memory */
static void SaveFile_ApplyEdits(SaveFile *sf) {
    for (int p = 0; p < sf->proficiencyCount; p++) {
        if (sf->proficiencies[p].newValue != sf->proficiencies[p].currentValue) {
            *(int32_t *)(sf->nodedata + sf->proficiencies[p].absPos) = sf->proficiencies[p].newValue;
        }
    }
}

/* Write the modified save to outputPath. Handles any number of dirty chunks,
   recompressing each once and cascading file-offset deltas correctly. */
int SaveFile_Write(SaveFile *sf, const char *outputPath) {
    SaveFile_ApplyEdits(sf);
    SaveFile_MarkDirtyChunks(sf);

    /* recompress each dirty chunk, compute new sizes */
    uint32_t *newSize = (uint32_t *)malloc(sf->chunkCount * sizeof(uint32_t));
    char **newCompData = (char **)calloc(sf->chunkCount, sizeof(char *));
    int *newCompLen = (int *)calloc(sf->chunkCount, sizeof(int));

    for (uint32_t i = 0; i < sf->chunkCount; i++) {
        if (sf->chunks[i].dirty) {
            int bound = LZ4_compressBound((int)sf->chunks[i].data_size);
            newCompData[i] = (char *)malloc((size_t)bound);
            newCompLen[i] = LZ4_compress_default(
                sf->nodedata + sf->chunks[i].data_offset_calc,
                newCompData[i], (int)sf->chunks[i].data_size, bound);
            if (newCompLen[i] <= 0) {
                snprintf(sf->lastError, sizeof(sf->lastError), "Recompression failed for chunk %u", i);
                free(newSize); free(newCompData); free(newCompLen);
                return 0;
            }
            newSize[i] = 8 + (uint32_t)newCompLen[i];
        } else {
            newSize[i] = sf->chunks[i].size;
        }
    }

    /* chunks are assumed to already be in file order matching their index
       (confirmed true for real save files - table stored pre-sorted) */
    FILE *out = fopen(outputPath, "wb");
    if (!out) {
        snprintf(sf->lastError, sizeof(sf->lastError), "Could not open output file");
        return 0;
    }

    fwrite(sf->rawFile, 1, (size_t)sf->chunkTableEntriesStart, out);

    /* cumulative delta as we go through chunks in file order */
    int64_t cumulativeDelta = 0;
    for (uint32_t i = 0; i < sf->chunkCount; i++) {
        uint32_t newOffset = (uint32_t)((int64_t)sf->chunks[i].offset + cumulativeDelta);
        fwrite(&newOffset, 4, 1, out);
        fwrite(&newSize[i], 4, 1, out);
        fwrite(&sf->chunks[i].data_size, 4, 1, out);
        cumulativeDelta += (int64_t)newSize[i] - (int64_t)sf->chunks[i].size;
    }

    /* write chunk data blocks in file order, using new compressed bytes for dirty ones */
    long cursor = sf->chunkTableEnd;
    for (uint32_t i = 0; i < sf->chunkCount; i++) {
        /* copy any gap before this chunk verbatim (padding/alignment, if any) */
        if ((long)sf->chunks[i].offset > cursor) {
            fwrite(sf->rawFile + cursor, 1, (size_t)((long)sf->chunks[i].offset - cursor), out);
        }
        if (sf->chunks[i].dirty) {
            uint32_t xlz4 = MAGIC_XLZ4;
            fwrite(&xlz4, 4, 1, out);
            fwrite(&sf->chunks[i].data_size, 4, 1, out);
            fwrite(newCompData[i], 1, (size_t)newCompLen[i], out);
        } else {
            fwrite(sf->rawFile + sf->chunks[i].offset, 1, sf->chunks[i].size, out);
        }
        cursor = (long)sf->chunks[i].offset + (long)sf->chunks[i].size;
    }

    /* copy from end of last chunk through to footer value, verbatim (node table etc) */
    if (sf->footerStart > cursor) {
        fwrite(sf->rawFile + cursor, 1, (size_t)(sf->footerStart - cursor), out);
    }

    uint32_t newNodedescsStart = (uint32_t)((int64_t)sf->nodedescsStart + cumulativeDelta);
    fwrite(&newNodedescsStart, 4, 1, out);
    uint32_t doneTag = MAGIC_DONE;
    fwrite(&doneTag, 4, 1, out);

    fclose(out);

    for (uint32_t i = 0; i < sf->chunkCount; i++) if (newCompData[i]) free(newCompData[i]);
    free(newCompData);
    free(newCompLen);
    free(newSize);

    return 1;
}

void SaveFile_Free(SaveFile *sf) {
    if (sf->rawFile) free(sf->rawFile);
    if (sf->descs) free(sf->descs);
    if (sf->chunks) free(sf->chunks);
    if (sf->nodedata) free(sf->nodedata);
}

#endif
