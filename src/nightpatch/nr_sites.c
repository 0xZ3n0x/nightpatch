/*
 * nr_sites.c - the site catalogue: where the sites are, and what a hit does.
 *
 * A pattern match alone is not enough to arm a breakpoint: the trap handler
 * rebuilds the stores by hand, so the instruction really has to be the exact
 * form it expects.  Everything here re-reads the site and checks it byte by
 * byte before the address is allowed anywhere near a debug register, then
 * describes the hit as one generic action for the framework to run.
 *
 * This is nightpatch's own knowledge; the machinery behind it comes from the
 * framework's public API (tk.h) and nothing else.
 */
#include <asm/ptrace.h>
#include <linux/kernel.h>
#include <linux/unaligned.h>

#include "nr_internal.h"

/* ------------------------------------------------------------------ */
/* Site patterns                                                       */
/* ------------------------------------------------------------------ */

/*
 * FRAMETICK primary: mov dword [reg+disp8], 1.0f/60 followed by a near jmp.
 * The immediate is 0x3C888889 little-endian, so bytes 4..6 of the match are
 * its top three bytes (the low 0x89 sits at index 3 and is left wild because
 * the fallback shares this decoder).
 *
 *   C7 /0 disp8 imm32   E9
 */
static const u8 frametick_primary_bytes[] = {0xC7, 0x00, 0x00, 0x00, 0x88, 0x88, 0x3C, 0xE9};
static const u8 frametick_primary_mask[] = {0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};

/*
 * FRAMETICK fallback: the same store, but reached by a short jmp instead of a
 * near one, so the primary's E9 anchor is absent.  Anchored instead on the
 * mov [rbx+disp8], esi that brackets it on both sides.  The store itself
 * begins three bytes into the match.
 *
 *   89 73 disp8 | C7 /0 disp8 imm32 | EB rel8 | 89 73
 */
static const u8 frametick_fallback_bytes[] = {0x89, 0x73, 0x00, 0xC7, 0x00, 0x00, 0x00,
                                              0x00, 0x00, 0x00, 0xEB, 0x00, 0x89, 0x73};
static const u8 frametick_fallback_mask[] = {0xFF, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00,
                                             0x00, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0xFF};

/*
 * HERTZ: the refresh-rate lock, two back-to-back stores of 60 and 1 into the
 * frame relative to rbp.  Runs at game boot and again every time the window
 * regains focus (e.g. alt-tab); the action re-zeroes the lock on each run.
 *
 *   C7 45 disp8 0000003C | C7 45 disp8 00000001
 */
static const u8 hertz_bytes[] = {0xC7, 0x45, 0x00, 0x3C, 0x00, 0x00, 0x00, 0xC7, 0x45, 0x00, 0x01, 0x00, 0x00, 0x00};
static const u8 hertz_mask[] = {0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};

/*
 * Patterns searched for, in the order the scanner tests them, primary-first.
 * FRAMETICK has two: the primary pins the 1/60 immediate directly, while the
 * fallback anchors on the surrounding instructions for builds that reach the
 * store via a short jump.  Both are scanned in the same pass and the primary
 * wins.
 */
enum
{
    NR_PATTERN_FRAMETICK_PRIMARY = 0,
    NR_PATTERN_FRAMETICK_FALLBACK,
    NR_PATTERN_HERTZ,
    NR_PATTERN_COUNT
};

struct nr_site_pattern
{
    struct tk_pattern pattern;
    int site_index;                  /* NP_SITE_* this pattern locates */
    unsigned int instruction_offset; /* match start -> first emulated instruction */
    bool fuzzy;                      /* set on results found via a fallback pattern */
    bool check_prefix;               /* reject an instruction prefix directly before the site */
};

static const struct nr_site_pattern nr_site_patterns[NR_PATTERN_COUNT] = {
    [NR_PATTERN_FRAMETICK_PRIMARY] = {.pattern = {.bytes = frametick_primary_bytes,
                                                  .mask = frametick_primary_mask,
                                                  .length = sizeof(frametick_primary_bytes),
                                                  .name = "frametick/primary"},
                                      .site_index = NP_SITE_FRAMETICK,
                                      .instruction_offset = 0,
                                      .fuzzy = false,
                                      .check_prefix = true},
    [NR_PATTERN_FRAMETICK_FALLBACK] = {.pattern = {.bytes = frametick_fallback_bytes,
                                                   .mask = frametick_fallback_mask,
                                                   .length = sizeof(frametick_fallback_bytes),
                                                   .name = "frametick/fallback"},
                                       .site_index = NP_SITE_FRAMETICK,
                                       .instruction_offset = 3,
                                       .fuzzy = true,
                                       .check_prefix = false},
    [NR_PATTERN_HERTZ] = {.pattern = {.bytes = hertz_bytes,
                                      .mask = hertz_mask,
                                      .length = sizeof(hertz_bytes),
                                      .name = "hertz"},
                          .site_index = NP_SITE_HERTZ,
                          .instruction_offset = 0,
                          .fuzzy = false,
                          .check_prefix = true},
};

/* ------------------------------------------------------------------ */
/* Intro logo movie names                                              */
/* ------------------------------------------------------------------ */

/*
 * The two startup logo movies are referenced by asset name, stored in the
 * image as NUL-terminated UTF-16.  Finding the name is only the first half of
 * finding the site: what gets trapped is the code that loads its address.
 *
 *   "05_901_Logo_BNE"       Bandai Namco
 *   "05_900_Logo_FromSoft"  FromSoftware
 *
 * Both are pinned byte for byte - they are long and highly specific, and a
 * wrong match here would send the LEA search hunting for a reference that does
 * not exist.  The leading 0x30 keeps the framework's scanner on its memchr
 * fast path.
 */
static const u8 intro_name_bne_bytes[] = {0x30, 0x00, 0x35, 0x00, 0x5F, 0x00, 0x39, 0x00, 0x30, 0x00,
                                          0x31, 0x00, 0x5F, 0x00, 0x4C, 0x00, 0x6F, 0x00, 0x67, 0x00,
                                          0x6F, 0x00, 0x5F, 0x00, 0x42, 0x00, 0x4E, 0x00, 0x45, 0x00,
                                          0x00, 0x00};
static const u8 intro_name_fromsoft_bytes[] = {0x30, 0x00, 0x35, 0x00, 0x5F, 0x00, 0x39, 0x00, 0x30, 0x00,
                                               0x30, 0x00, 0x5F, 0x00, 0x4C, 0x00, 0x6F, 0x00, 0x67, 0x00,
                                               0x6F, 0x00, 0x5F, 0x00, 0x46, 0x00, 0x72, 0x00, 0x6F, 0x00,
                                               0x6D, 0x00, 0x53, 0x00, 0x6F, 0x00, 0x66, 0x00, 0x74, 0x00,
                                               0x00, 0x00};

/* Both names are pinned byte for byte, so one all-0xFF mask serves both; it
 * only has to be at least as long as the longest of them. */
static const u8 intro_name_mask[] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
};

/* The intro logo movies, in the order the module hunts for them. */
enum
{
    NR_INTRO_BNE = 0,
    NR_INTRO_FROMSOFT,
    NR_INTRO_COUNT
};

/*
 * One intro movie: the UTF-16 asset name to find in the image's data, and the
 * site that the code reference to it becomes.
 */
struct nr_intro_movie
{
    struct tk_pattern name_pattern;
    int site_index; /* NP_SITE_INTRO_* */
};

static const struct nr_intro_movie nr_intro_movies[NR_INTRO_COUNT] = {
    [NR_INTRO_BNE] = {.name_pattern = {.bytes = intro_name_bne_bytes,
                                       .mask = intro_name_mask,
                                       .length = sizeof(intro_name_bne_bytes),
                                       .name = "intro/name/bne"},
                      .site_index = NP_SITE_INTRO_BNE},
    [NR_INTRO_FROMSOFT] = {.name_pattern = {.bytes = intro_name_fromsoft_bytes,
                                            .mask = intro_name_mask,
                                            .length = sizeof(intro_name_fromsoft_bytes),
                                            .name = "intro/name/fromsoft"},
                           .site_index = NP_SITE_INTRO_FROMSOFT},
};

/* ------------------------------------------------------------------ */
/* Intro code references                                               */
/* ------------------------------------------------------------------ */

#define OPCODE_LEA_R64_M 0x8D      /* 48 8D /r, with the REX.W below */
#define OPCODE_MOV_RM64_R64 0x89   /* REX.W 89 /r: mov r/m64, r64 */
#define OPCODE_NOP 0x90
#define REX_W_PREFIX 0x48          /* REX.W with no register extensions */
#define MODRM_RIP_RELATIVE_RAX 0x05 /* mod=00, reg=RAX, rm=101 (disp32 from RIP) */
#define MODRM_REG_RAX 0

#define LEA_INSN_LENGTH 7 /* REX.W + opcode + modrm + disp32 */

/*
 * How far past the LEA to look for the store that consumes it, and how many
 * bytes that store needs.  Together with the LEA they fix the pattern length,
 * which is what guarantees the framework's scanner has the whole window
 * buffered when it calls the verifier - no reads from inside the scan loop.
 */
#define LEA_STORE_LOOKAHEAD 32
#define MOV_STORE_PREFIX_LENGTH 3 /* REX.W + 0x89 + modrm */
#define INTRO_LEA_PATTERN_LENGTH (LEA_INSN_LENGTH + LEA_STORE_LOOKAHEAD + MOV_STORE_PREFIX_LENGTH)

/* Only the opcode is fixed; the displacement is what the verifier resolves,
 * and everything past the LEA is context it walks by hand. */
static const u8 intro_lea_bytes[INTRO_LEA_PATTERN_LENGTH] = {REX_W_PREFIX, OPCODE_LEA_R64_M, MODRM_RIP_RELATIVE_RAX};
static const u8 intro_lea_mask[INTRO_LEA_PATTERN_LENGTH] = {0xFF, 0xFF, 0xFF};

static const char* const intro_lea_pattern_names[NR_INTRO_COUNT] = {
    [NR_INTRO_BNE] = "intro/lea/bne",
    [NR_INTRO_FROMSOFT] = "intro/lea/fromsoft",
};

/*
 * Context for an intro LEA pattern: the address its displacement has to
 * resolve to.  The caller owns this and must keep it alive for as long as the
 * pattern built from it is in use.
 */
struct nr_lea_context
{
    unsigned long name_address;
};

/* REX with W set: the 64-bit operand size the store must use. */
static bool is_rex_w(u8 byte)
{
    return (byte & 0xF0) == 0x40 && (byte & 0x08);
}

/* Where a "lea rax, [rip+disp32]" at this address actually points. */
static unsigned long lea_target(const u8* code, unsigned long address)
{
    s32 displacement = (s32)get_unaligned_le32(code + 3);

    return address + LEA_INSN_LENGTH + (unsigned long)(long)displacement;
}

/*
 * Decide whether a candidate "lea rax, [rip+disp32]" is the one that loads
 * this movie's name.
 *
 * Two conditions, both from the reference implementation.  The displacement
 * has to resolve to the name, and the pointer has to be stored somewhere
 * shortly afterwards - the store is what distinguishes the reference that
 * feeds the movie-name field from an incidental mention of the same string.
 *
 * The store test is a sliding byte window, not a decoder: it accepts a
 * REX.W 89 /r (reg = RAX) byte sequence starting at any offset in the
 * lookahead, including inside another instruction, and does not check that
 * RAX survives until then.  That is deliberately loose - tightening it to
 * "first non-NOP instruction" would need the real game's bytes to confirm it
 * still matches, and the cost of a loose pick is bounded: the displacement
 * already pins the identity, and these sites write nothing.
 * A LEA that fails the second test is rejected and the scan carries on, so a
 * later, better reference can still win.
 *
 * Context: called from inside the scan loop; pure, reads only *match.
 */
static bool lea_loads_movie_name(const u8* match, unsigned long va, const void* context)
{
    const struct nr_lea_context* wanted = context;
    unsigned int offset;

    if (lea_target(match, va) != wanted->name_address)
    {
        return false;
    }

    for (offset = LEA_INSN_LENGTH; offset < LEA_INSN_LENGTH + LEA_STORE_LOOKAHEAD; offset++)
    {
        const u8* insn = match + offset;

        if (insn[0] == OPCODE_NOP)
        {
            continue; /* alignment padding between the two */
        }
        if (is_rex_w(insn[0]) && insn[1] == OPCODE_MOV_RM64_R64 && ((insn[2] >> 3) & 7) == MODRM_REG_RAX)
        {
            return true;
        }
    }
    return false;
}

/*
 * Build the pattern that finds the "lea rax, [rip+disp32]" loading the given
 * movie name.  Fills both *out and *context; *out points into *context, so the
 * two must have the same lifetime.
 */
static void build_intro_lea_pattern(struct tk_pattern* out, struct nr_lea_context* context, unsigned int intro_index,
                                    unsigned long name_address)
{
    context->name_address = name_address;

    memset(out, 0, sizeof(*out));
    out->bytes = intro_lea_bytes;
    out->mask = intro_lea_mask;
    out->length = INTRO_LEA_PATTERN_LENGTH;
    out->name = intro_lea_pattern_names[intro_index];
    out->verify = lea_loads_movie_name;
    out->context = context;
}

/* ------------------------------------------------------------------ */
/* Instruction decoding: verify, then describe the hit as an action     */
/* ------------------------------------------------------------------ */

/* Length of the encodings we act on. */
#define FRAMETICK_INSN_LENGTH 7 /* opcode + modrm + disp8 + imm32 */
#define HERTZ_INSN_LENGTH 14    /* two such stores back to back */

/* Immediates the untouched game stores at each site. */
#define FRAMETICK_ORIGINAL_IMM 0x3C888889 /* float32 1.0f/60 */
#define HERTZ_FIRST_IMM 60
#define HERTZ_SECOND_IMM 1

/* One byte of context before the site plus the longest encoding we decode. */
#define NR_DECODE_WINDOW 17
#define NR_DECODE_PREFIX_SLOT 1 /* index of the site's first byte inside the window */

static const char* nr_site_name(const struct tk_target* t, int site_index)
{
    if (!t || site_index < 0 || (unsigned int)site_index >= t->site_count)
    {
        return "?";
    }
    return t->sites[site_index].name;
}

/*
 * mov dword [base+disp8], imm32 where the immediate is the stock 1/60 frame
 * time.  Verifying the immediate matters most for the fallback pattern, which
 * wildcards it entirely and would otherwise accept any C7 store sitting
 * between the two anchor instructions.
 */
static int nr_decode_frametick(const u8* code, unsigned long address, struct tk_action* out)
{
    u8 base_reg;
    s8 disp;
    u32 immediate;
    int ret = tk_x86_decode_mov_m32_imm32(code, &base_reg, &disp, &immediate);

    if (ret)
    {
        pr_warn("nightpatch: frametick at %#lx: not a bare mod=01 C7 store (%d)\n", address, ret);
        return ret;
    }
    if (immediate != FRAMETICK_ORIGINAL_IMM)
    {
        pr_warn("nightpatch: frametick at %#lx: immediate %#010x, expected %#010x\n", address, immediate,
                FRAMETICK_ORIGINAL_IMM);
        return -EINVAL;
    }

    /* Replace the stock 1/60 frame time with the target frame time, which
     * nr_set_fps() keeps current through the live pointer. */
    out->kind = TK_ACT_STORE32;
    out->skip_len = FRAMETICK_INSN_LENGTH;
    out->store_count = 1;
    out->stores[0].base_reg = base_reg;
    out->stores[0].disp = disp;
    out->stores[0].live = &nr_frame_time_bits;
    return 0;
}

/* Two consecutive mov dword [rbp+disp8], imm32 storing 60 then 1. */
static int nr_decode_hertz(const u8* code, unsigned long address, struct tk_action* out)
{
    u8 base_reg;
    s8 disp;
    u32 immediate;
    s8 first_disp;
    const u8 rbp = tk_x86_modrm_rm_to_regs(5);

    if (tk_x86_decode_mov_m32_imm32(code, &base_reg, &disp, &immediate) || base_reg != rbp ||
        immediate != HERTZ_FIRST_IMM)
    {
        pr_warn("nightpatch: hertz at %#lx: first store is not a [rbp+disp8] store of %u\n", address, HERTZ_FIRST_IMM);
        return -EINVAL;
    }
    first_disp = disp;
    if (tk_x86_decode_mov_m32_imm32(code + FRAMETICK_INSN_LENGTH, &base_reg, &disp, &immediate) || base_reg != rbp ||
        immediate != HERTZ_SECOND_IMM)
    {
        pr_warn("nightpatch: hertz at %#lx: second store is not a [rbp+disp8] store of %u\n", address,
                HERTZ_SECOND_IMM);
        return -EINVAL;
    }

    /* Zeroing both fields is what stops the game pinning itself to the
     * 60 Hz path. */
    out->kind = TK_ACT_STORE32;
    out->skip_len = HERTZ_INSN_LENGTH;
    out->store_count = 2;
    out->stores[0].base_reg = rbp;
    out->stores[0].disp = first_disp;
    out->stores[0].imm = 0;
    out->stores[1].base_reg = rbp;
    out->stores[1].disp = disp;
    out->stores[1].imm = 0;
    return 0;
}

/*
 * lea rax, [rip+disp32] loading a movie name.
 *
 * Nothing is substituted here - the trap handler just steps over the LEA,
 * which leaves exactly the machine state seven NOPs would - so the only thing
 * that has to be right is that this really is that LEA and that it really
 * points at the name the scanner matched.  Re-resolving the displacement
 * against the name catches a stale address from a previous session outright.
 */
static int nr_decode_intro(const u8* code, unsigned long address, unsigned long expected_target,
                           struct tk_action* out)
{
    unsigned long target;

    if (code[0] != REX_W_PREFIX || code[1] != OPCODE_LEA_R64_M || code[2] != MODRM_RIP_RELATIVE_RAX)
    {
        pr_warn("nightpatch: intro site at %#lx: not a lea rax, [rip+disp32]\n", address);
        return -EINVAL;
    }

    target = lea_target(code, address);
    if (!expected_target || target != expected_target)
    {
        pr_warn("nightpatch: intro site at %#lx: resolves to %#lx, expected %#lx\n", address, target,
                expected_target);
        return -EINVAL;
    }

    out->kind = TK_ACT_SKIP;
    out->skip_len = LEA_INSN_LENGTH;
    out->store_count = 0;
    return 0;
}

/*
 * Read the instruction at address, confirm it is exactly the encoding this
 * module knows how to act on, fill *out with the action it becomes, and hand
 * the site to the framework.
 *
 * Generally the byte before the site is rejected if it is an instruction
 * prefix; the framing fallback is the exception: its pattern pins the
 * instruction before the store (89 73 disp8), so the byte in question is that
 * disp8, and a value of 0x40-0x4F there is an ordinary displacement, not a
 * REX prefix.  The intro sites are not tested either: their identity is
 * already established by a displacement that resolves to the exact movie
 * name, and nothing about them is emulated - the lea is only stepped over.
 * Rejecting them would trade a real site for a coincidence: the byte before
 * an instruction is the tail of the previous one, and 0x48 in particular is
 * everywhere in x86-64 code.  The failure the check guards against is also
 * harmless for them, because an execute breakpoint on an address that is not
 * an instruction boundary simply never fires.
 *
 * expected_lea_target is the address an intro site's LEA must resolve to; it
 * is ignored by, and should be passed as 0 for, every other site.
 *
 * Context: sleepable (reads game memory), called under the framework's
 * target lock from within locate().
 * Returns 0, or -EFAULT if the bytes could not be read, or -EINVAL/-EEXIST
 * if the encoding is not the expected one or the site is already resolved.
 */
static int nr_decode_and_resolve(struct tk_target* t, const struct tk_image* img,
                                 const struct nr_site_pattern* entry, unsigned long address,
                                 unsigned long expected_lea_target)
{
    u8 window[NR_DECODE_WINDOW];
    const u8* code = window + NR_DECODE_PREFIX_SLOT;
    struct tk_action action;
    int site_index = entry->site_index;
    int ret;

    memset(&action, 0, sizeof(action));

    if (!tk_address_plausible(address) || address < NR_DECODE_PREFIX_SLOT)
    {
        return -EINVAL;
    }

    /* One byte before the site so the preceding prefix can be inspected. */
    if (tk_read_remote_exact(img->task, address - NR_DECODE_PREFIX_SLOT, window, sizeof(window)))
    {
        pr_warn("nightpatch: %s site at %#lx: cannot read %d bytes of code\n", nr_site_name(t, site_index), address,
                NR_DECODE_WINDOW);
        return -EFAULT;
    }

    /* See the function comment for why the prefix test is per-pattern. */
    if (entry->check_prefix && tk_x86_is_prefix(window[0]))
    {
        pr_warn("nightpatch: %s site at %#lx: prefix byte %#04x precedes the site\n", nr_site_name(t, site_index),
                address, window[0]);
        return -EINVAL;
    }

    if (site_index == NP_SITE_FRAMETICK)
    {
        ret = nr_decode_frametick(code, address, &action);
    }
    else if (site_index == NP_SITE_HERTZ)
    {
        ret = nr_decode_hertz(code, address, &action);
    }
    else /* the intro sites */
    {
        ret = nr_decode_intro(code, address, expected_lea_target, &action);
    }
    if (ret)
    {
        return ret;
    }

    if (entry->fuzzy)
    {
        action.flags |= TK_ACTION_FUZZY;
    }
    return tk_site_resolve(t, (unsigned int)site_index, address, &action);
}

/* ------------------------------------------------------------------ */
/* locate()                                                            */
/* ------------------------------------------------------------------ */

/*
 * Locate and resolve the frame-rate sites, all from one scan: the hertz site
 * executes exactly once, during boot, so single pass is what wins that race.
 *
 * Patterns are listed primary-first, so the first one that matches and
 * decodes wins for its site and later fallbacks are ignored - which is also
 * why resolve failures other than "already resolved" simply continue.
 */
static int nr_locate_required(struct tk_target* t, const struct tk_image* img)
{
    struct tk_pattern patterns[NR_PATTERN_COUNT];
    struct tk_scan_result results[NR_PATTERN_COUNT];
    unsigned int matched;
    unsigned int i;

    for (i = 0; i < NR_PATTERN_COUNT; i++)
    {
        patterns[i] = nr_site_patterns[i].pattern;
    }

    matched = tk_scan_ranges(img->task, img->exec, img->exec_count, patterns, NR_PATTERN_COUNT, results,
                              img->scan_buffer, img->scan_buffer_size);
    if (!matched)
    {
        pr_warn("nightpatch: no patch site patterns matched; leaving the game alone\n");
        return -ENOENT;
    }

    for (i = 0; i < NR_PATTERN_COUNT; i++)
    {
        const struct nr_site_pattern* entry = &nr_site_patterns[i];

        if (!results[i].found)
        {
            continue;
        }
        nr_decode_and_resolve(t, img, entry, results[i].address + entry->instruction_offset, 0);
    }
    /* The framework checks that every REQUIRED site resolved. */
    return 0;
}

/*
 * Locate and resolve the two intro sites.
 *
 * Unlike the frame-rate sites these cannot be located by their bytes alone: a
 * "lea rax, [rip+disp32]" only reveals what it loads once you know where the
 * string is, so this takes two scans - the movie names in the image's data
 * mappings, then the code that points at them.
 *
 * Every failure is survivable and none of them abandons the game: the worst
 * case is that a logo plays.
 *
 * Context: sleepable, under the framework's target lock.
 */
static int nr_locate_optional(struct tk_target* t, const struct tk_image* img)
{
    struct tk_pattern name_patterns[NR_INTRO_COUNT];
    struct tk_scan_result name_results[NR_INTRO_COUNT];
    struct nr_lea_context lea_contexts[NR_INTRO_COUNT];
    struct tk_pattern lea_patterns[NR_INTRO_COUNT];
    struct tk_scan_result lea_results[NR_INTRO_COUNT];
    unsigned int intro_of_lea[NR_INTRO_COUNT];
    unsigned int lea_count = 0;
    unsigned int found;
    unsigned int i;

    for (i = 0; i < NR_INTRO_COUNT; i++)
    {
        name_patterns[i] = nr_intro_movies[i].name_pattern;
    }

    /*
     * The names are string literals, so a read-only data mapping is where they
     * belong and where they are cheapest to find.  If that comes up short the
     * executable mappings are tried too: how a PE's sections end up carved
     * into VMAs is the loader's business, not ours, and a name sitting in a
     * mapping that happens to also be executable is not a reason to give up.
     */
    found = tk_scan_ranges(img->task, img->data, img->data_count, name_patterns, NR_INTRO_COUNT, name_results,
                            img->scan_buffer, img->scan_buffer_size);
    if (found < NR_INTRO_COUNT)
    {
        struct tk_scan_result retry[NR_INTRO_COUNT];

        tk_scan_ranges(img->task, img->exec, img->exec_count, name_patterns, NR_INTRO_COUNT, retry, img->scan_buffer,
                        img->scan_buffer_size);
        for (i = 0; i < NR_INTRO_COUNT; i++)
        {
            if (!name_results[i].found && retry[i].found)
            {
                name_results[i] = retry[i];
            }
        }
    }

    /* Only hunt for code references to names that actually exist; a pattern
     * built around a name we never found could not match anything anyway. */
    for (i = 0; i < NR_INTRO_COUNT; i++)
    {
        if (!name_results[i].found)
        {
            pr_warn("nightpatch: %s not present in the image; that logo will play\n", name_patterns[i].name);
            continue;
        }
        build_intro_lea_pattern(&lea_patterns[lea_count], &lea_contexts[lea_count], i, name_results[i].address);
        intro_of_lea[lea_count] = i;
        lea_count++;
    }
    if (!lea_count)
    {
        return 0;
    }

    tk_scan_ranges(img->task, img->exec, img->exec_count, lea_patterns, lea_count, lea_results, img->scan_buffer,
                    img->scan_buffer_size);

    for (i = 0; i < lea_count; i++)
    {
        const struct nr_intro_movie* movie = &nr_intro_movies[intro_of_lea[i]];
        unsigned long name_address = name_results[intro_of_lea[i]].address;
        unsigned long address = lea_results[i].address;
        int ret;

        if (!lea_results[i].found)
        {
            pr_warn("nightpatch: no code reference to the name at %#lx; %s logo will play\n", name_address,
                    nr_site_name(t, movie->site_index));
            continue;
        }
        ret = nr_decode_and_resolve(t, img,
                                    &(const struct nr_site_pattern){
                                        .pattern = lea_patterns[i],
                                        .site_index = movie->site_index,
                                        .instruction_offset = 0,
                                    },
                                    address, name_address);
        if (ret)
        {
            continue; /* the decoder, or the framework, already explained why */
        }

        pr_info("nightpatch: %s located at %#lx (name at %#lx)\n", nr_site_name(t, movie->site_index), address,
                name_address);
    }
    return 0;
}

int nr_locate(struct tk_target* t, const struct tk_image* img, enum tk_phase phase)
{
    if (phase == TK_PHASE_REQUIRED)
    {
        return nr_locate_required(t, img);
    }
    return nr_locate_optional(t, img);
}
