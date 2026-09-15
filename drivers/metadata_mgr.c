#include <stdint.h>
#include "metadata_mgr.h"
#include "flash.h"
#include "crc.h"

/* The CRC covers everything except itself: the first 44 bytes. */
#define META_CRC_COVERAGE   (sizeof(metadata_t) - sizeof(uint32_t))

static const uint32_t meta_pages[2] = {
    META_PAGE_A_ADDR,
    META_PAGE_B_ADDR
};


/* ----------------------------------------------------------------
 * Validation
 * ---------------------------------------------------------------- */

int metadata_is_valid(const metadata_t *meta)
{
    if (meta == 0) {
        return 0;
    }

    if (meta->magic != METADATA_MAGIC) {
        return 0;
    }

    /* The CRC handles cases the magic cannot detect:
       interrupted write, degraded cell, partial erase. */
    uint32_t computed = crc32_compute((const uint8_t *)meta,
                                     META_CRC_COVERAGE);

    return (computed == meta->meta_crc32);
}


/* ----------------------------------------------------------------
 * Read
 * ---------------------------------------------------------------- */

/*
 * Loads a page and indicates whether it is usable.
 * The copy is made before validation: reading from flash then
 * validating the copy avoids reading the same region twice.
 */
static int load_page(unsigned index, metadata_t *dest)
{
    const metadata_t *src = (const metadata_t *)meta_pages[index];

    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dest;
    for (unsigned i = 0; i < sizeof(metadata_t); i++) {
        d[i] = s[i];
    }

    return metadata_is_valid(dest);
}


int metadata_read(metadata_t *dest)
{
    if (dest == 0) {
        return 0;
    }

    metadata_t a;
    metadata_t b;

    int a_ok = load_page(0, &a);
    int b_ok = load_page(1, &b);

    if (!a_ok && !b_ok) {
        return 0;               /* blank board or destroyed metadata */
    }

    const metadata_t *chosen;

    if (a_ok && b_ok) {
        /* Strict comparison: on equal counters, page 0 wins.
           See the note on the equal-counter case in metadata_mgr.h. */
        chosen = (b.counter > a.counter) ? &b : &a;
    } else {
        chosen = a_ok ? &a : &b;
    }

    const uint8_t *s = (const uint8_t *)chosen;
    uint8_t *d = (uint8_t *)dest;
    for (unsigned i = 0; i < sizeof(metadata_t); i++) {
        d[i] = s[i];
    }

    return 1;
}


/* ----------------------------------------------------------------
 * Write
 * ---------------------------------------------------------------- */

/*
 * Determines which page to write to and what counter to use.
 *
 * Always targets the page that does NOT hold the current version:
 * it can be safely erased since the other remains readable
 * throughout the operation.
 */
static void select_target(unsigned *page, uint32_t *counter)
{
    metadata_t a;
    metadata_t b;

    int a_ok = load_page(0, &a);
    int b_ok = load_page(1, &b);

    if (!a_ok && !b_ok) {
        *page = 0;
        *counter = 1;
        return;
    }

    if (a_ok && b_ok) {
        if (b.counter > a.counter) {
            *page = 0;                  /* B is current -> write to A */
            *counter = b.counter + 1U;
        } else {
            *page = 1;                  /* A is current -> write to B */
            *counter = a.counter + 1U;
        }
        return;
    }

    if (a_ok) {
        *page = 1;
        *counter = a.counter + 1U;
    } else {
        *page = 0;
        *counter = b.counter + 1U;
    }
}


metadata_status_t metadata_write(const metadata_t *src)
{
    if (src == 0) {
        return META_ERR_ARG;
    }

    unsigned page;
    uint32_t counter;
    select_target(&page, &counter);

    /* Full construction from zero.
     *
     * Zero-initialisation ensures that reserved bytes and any
     * potential padding are zero, keeping memory dumps readable and
     * making it possible to distinguish, when a reserved byte is
     * eventually used, a written value from a residue.
     *
     * magic, counter and meta_crc32 are computed here, not copied
     * from src: the caller must not be able to produce a structure
     * with a wrong CRC or an inconsistent counter.
     *
     * The zeroing is written as an explicit loop rather than using
     * a { 0 } initialiser. For a structure of this size, GCC would
     * replace the latter with a call to memset(), which does not
     * exist when compiling with -nostdlib. */
    metadata_t out;
    {
        uint8_t *raw = (uint8_t *)&out;
        for (unsigned i = 0; i < sizeof(out); i++) {
            raw[i] = 0;
        }
    }

    out.magic           = METADATA_MAGIC;
    out.counter         = counter;
    out.active_slot     = src->active_slot;
    out.boot_fail_count = src->boot_fail_count;

    for (unsigned s = 0; s < 2U; s++) {
        out.slot[s].size    = src->slot[s].size;
        out.slot[s].crc32   = src->slot[s].crc32;
        out.slot[s].version = src->slot[s].version;
        out.slot[s].state   = src->slot[s].state;
    }

    out.meta_crc32 = crc32_compute((const uint8_t *)&out,
                                   META_CRC_COVERAGE);

    uint32_t addr = meta_pages[page];

    if (flash_erase_page(addr) != FLASH_OK) {
        return META_ERR_ERASE;
    }

    /* sizeof(metadata_t) is 48, a multiple of the write unit. */
    if (flash_write(addr, (const uint8_t *)&out,
                    sizeof(metadata_t)) != FLASH_OK) {
        return META_ERR_WRITE;
    }

    /* Full re-read: flash_write() already did its own read-back,
       but here we verify that the re-read structure is recognised as
       valid by the same criteria used at startup. */
    metadata_t reread;
    if (!load_page(page, &reread)) {
        return META_ERR_VERIFY;
    }
    if (reread.counter != counter) {
        return META_ERR_VERIFY;
    }

    return META_OK;
}


/* ----------------------------------------------------------------
 * Full erase
 * ---------------------------------------------------------------- */

metadata_status_t metadata_erase_all(void)
{
    for (unsigned i = 0; i < 2; i++) {
        if (flash_erase_page(meta_pages[i]) != FLASH_OK) {
            return META_ERR_ERASE;
        }
    }
    return META_OK;
}
