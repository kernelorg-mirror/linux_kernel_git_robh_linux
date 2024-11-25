// SPDX-License-Identifier: GPL-2.0-only
/*
 * Branch Record Buffer Extension Driver.
 *
 * Copyright (C) 2022-2023 ARM Limited
 *
 * Author: Anshuman Khandual <anshuman.khandual@arm.com>
 */
#include <linux/perf/arm_pmu.h>
#include "arm_brbe.h"

#define BRBFCR_EL1_BRANCH_FILTERS (BRBFCR_EL1_DIRECT   | \
				   BRBFCR_EL1_INDIRECT | \
				   BRBFCR_EL1_RTN      | \
				   BRBFCR_EL1_INDCALL  | \
				   BRBFCR_EL1_DIRCALL  | \
				   BRBFCR_EL1_CONDDIR)

#define BRBFCR_EL1_CONFIG_MASK    (BRBFCR_EL1_BANK_MASK | \
				   BRBFCR_EL1_PAUSED    | \
				   BRBFCR_EL1_EnI       | \
				   BRBFCR_EL1_BRANCH_FILTERS)

/*
 * BRBTS_EL1 is currently not used for branch stack implementation
 * purpose but BRBCR_ELx.TS needs to have a valid value from all
 * available options. BRBCR_ELx_TS_VIRTUAL is selected for this.
 */
#define BRBCR_ELx_DEFAULT_TS      FIELD_PREP(BRBCR_ELx_TS_MASK, BRBCR_ELx_TS_VIRTUAL)

#define BRBCR_ELx_CONFIG_MASK     (BRBCR_ELx_EXCEPTION | \
				   BRBCR_ELx_ERTN      | \
				   BRBCR_ELx_CC        | \
				   BRBCR_ELx_MPRED     | \
				   BRBCR_ELx_ExBRE     | \
				   BRBCR_ELx_E0BRE     | \
				   BRBCR_ELx_FZP       | \
				   BRBCR_ELx_TS_MASK)

/*
 * BRBE Buffer Organization
 *
 * BRBE buffer is arranged as multiple banks of 32 branch record
 * entries each. An individual branch record in a given bank could
 * be accessed, after selecting the bank in BRBFCR_EL1.BANK and
 * accessing the registers i.e [BRBSRC, BRBTGT, BRBINF] set with
 * indices [0..31].
 *
 * Bank 0
 *
 *	---------------------------------	------
 *	| 00 | BRBSRC | BRBTGT | BRBINF |	| 00 |
 *	---------------------------------	------
 *	| 01 | BRBSRC | BRBTGT | BRBINF |	| 01 |
 *	---------------------------------	------
 *	| .. | BRBSRC | BRBTGT | BRBINF |	| .. |
 *	---------------------------------	------
 *	| 31 | BRBSRC | BRBTGT | BRBINF |	| 31 |
 *	---------------------------------	------
 *
 * Bank 1
 *
 *	---------------------------------	------
 *	| 32 | BRBSRC | BRBTGT | BRBINF |	| 00 |
 *	---------------------------------	------
 *	| 33 | BRBSRC | BRBTGT | BRBINF |	| 01 |
 *	---------------------------------	------
 *	| .. | BRBSRC | BRBTGT | BRBINF |	| .. |
 *	---------------------------------	------
 *	| 63 | BRBSRC | BRBTGT | BRBINF |	| 31 |
 *	---------------------------------	------
 */
#define BRBE_BANK_MAX_ENTRIES	32
#define BRBE_MAX_BANK		2
#define BRBE_MAX_ENTRIES	(BRBE_BANK_MAX_ENTRIES * BRBE_MAX_BANK)

struct brbe_regset {
	unsigned long brbsrc;
	unsigned long brbtgt;
	unsigned long brbinf;
};

#define PERF_BR_ARM64_MAX (PERF_BR_MAX + PERF_BR_NEW_MAX)

static void branch_mask_set_all(unsigned long *event_type_mask)
{
	bitmap_set(event_type_mask, PERF_BR_UNKNOWN, PERF_BR_SERROR + 1);
	bitmap_set(event_type_mask, PERF_BR_MAX + PERF_BR_NEW_FAULT_ALGN, PERF_BR_NEW_MAX);
}

static void branch_mask_set_arch(unsigned long *event_type_mask)
{
	set_bit(PERF_BR_MAX + PERF_BR_NEW_FAULT_ALGN, event_type_mask);
	set_bit(PERF_BR_MAX + PERF_BR_NEW_FAULT_DATA, event_type_mask);
	set_bit(PERF_BR_MAX + PERF_BR_NEW_FAULT_INST, event_type_mask);

	set_bit(PERF_BR_MAX + PERF_BR_ARM64_FIQ, event_type_mask);
	set_bit(PERF_BR_MAX + PERF_BR_ARM64_DEBUG_HALT, event_type_mask);
	set_bit(PERF_BR_MAX + PERF_BR_ARM64_DEBUG_EXIT, event_type_mask);
	set_bit(PERF_BR_MAX + PERF_BR_ARM64_DEBUG_INST, event_type_mask);
	set_bit(PERF_BR_MAX + PERF_BR_ARM64_DEBUG_DATA, event_type_mask);
}

static void branch_entry_mask(struct perf_branch_entry *entry,
			      unsigned long *event_type_mask)
{
	bitmap_zero(event_type_mask, PERF_BR_ARM64_MAX);
	if (entry->type < PERF_BR_EXTEND_ABI)
		set_bit(entry->type, event_type_mask);
	else if (entry->new_type < PERF_BR_NEW_MAX)
		set_bit(PERF_BR_MAX + entry->new_type, event_type_mask);
}

static void prepare_event_branch_type_mask(struct perf_event *event,
					   unsigned long *event_type_mask)
{
	u64 branch_sample = event->attr.branch_sample_type;

	bitmap_zero(event_type_mask, PERF_BR_ARM64_MAX);

	/*
	 * The platform specific branch types might not follow event's
	 * branch filter requests accurately. Let's add all of them as
	 * acceptible branch types during the filtering process.
	 */
	branch_mask_set_arch(event_type_mask);

	if (branch_sample & PERF_SAMPLE_BRANCH_ANY) {
		branch_mask_set_all(event_type_mask);
		return;
	}

	if (branch_sample & PERF_SAMPLE_BRANCH_IND_JUMP)
		set_bit(PERF_BR_IND, event_type_mask);

	set_bit(PERF_BR_UNCOND, event_type_mask);
	if (branch_sample & PERF_SAMPLE_BRANCH_COND) {
		clear_bit(PERF_BR_UNCOND, event_type_mask);
		set_bit(PERF_BR_COND, event_type_mask);
	}

	if (branch_sample & PERF_SAMPLE_BRANCH_CALL)
		set_bit(PERF_BR_CALL, event_type_mask);

	if (branch_sample & PERF_SAMPLE_BRANCH_IND_CALL)
		set_bit(PERF_BR_IND_CALL, event_type_mask);

	if (branch_sample & PERF_SAMPLE_BRANCH_ANY_CALL) {
		set_bit(PERF_BR_CALL, event_type_mask);
		set_bit(PERF_BR_IRQ, event_type_mask);
		set_bit(PERF_BR_SYSCALL, event_type_mask);
		set_bit(PERF_BR_SERROR, event_type_mask);

		if (branch_sample & PERF_SAMPLE_BRANCH_COND)
			set_bit(PERF_BR_COND_CALL, event_type_mask);
	}

	if (branch_sample & PERF_SAMPLE_BRANCH_ANY_RETURN) {
		set_bit(PERF_BR_RET, event_type_mask);
		set_bit(PERF_BR_ERET, event_type_mask);
		set_bit(PERF_BR_SYSRET, event_type_mask);

		if (branch_sample & PERF_SAMPLE_BRANCH_COND)
			set_bit(PERF_BR_COND_RET, event_type_mask);
	}
}

struct brbe_hw_attr {
	int	brbe_version;
	int	brbe_cc;
	int	brbe_nr;
	int	brbe_format;
};

#define BRBE_REGN_CASE(n, case_macro) \
	case n: case_macro(n); break

#define BRBE_REGN_SWITCH(x, case_macro)				\
	do {							\
		switch (x) {					\
		BRBE_REGN_CASE(0, case_macro);			\
		BRBE_REGN_CASE(1, case_macro);			\
		BRBE_REGN_CASE(2, case_macro);			\
		BRBE_REGN_CASE(3, case_macro);			\
		BRBE_REGN_CASE(4, case_macro);			\
		BRBE_REGN_CASE(5, case_macro);			\
		BRBE_REGN_CASE(6, case_macro);			\
		BRBE_REGN_CASE(7, case_macro);			\
		BRBE_REGN_CASE(8, case_macro);			\
		BRBE_REGN_CASE(9, case_macro);			\
		BRBE_REGN_CASE(10, case_macro);			\
		BRBE_REGN_CASE(11, case_macro);			\
		BRBE_REGN_CASE(12, case_macro);			\
		BRBE_REGN_CASE(13, case_macro);			\
		BRBE_REGN_CASE(14, case_macro);			\
		BRBE_REGN_CASE(15, case_macro);			\
		BRBE_REGN_CASE(16, case_macro);			\
		BRBE_REGN_CASE(17, case_macro);			\
		BRBE_REGN_CASE(18, case_macro);			\
		BRBE_REGN_CASE(19, case_macro);			\
		BRBE_REGN_CASE(20, case_macro);			\
		BRBE_REGN_CASE(21, case_macro);			\
		BRBE_REGN_CASE(22, case_macro);			\
		BRBE_REGN_CASE(23, case_macro);			\
		BRBE_REGN_CASE(24, case_macro);			\
		BRBE_REGN_CASE(25, case_macro);			\
		BRBE_REGN_CASE(26, case_macro);			\
		BRBE_REGN_CASE(27, case_macro);			\
		BRBE_REGN_CASE(28, case_macro);			\
		BRBE_REGN_CASE(29, case_macro);			\
		BRBE_REGN_CASE(30, case_macro);			\
		BRBE_REGN_CASE(31, case_macro);			\
		default: WARN(1, "Invalid BRB* index %d\n", x);	\
		}						\
	} while (0)

#define RETURN_READ_BRBSRCN(n) \
	return read_sysreg_s(SYS_BRBSRC_EL1(n))
static inline u64 get_brbsrc_reg(int idx)
{
	BRBE_REGN_SWITCH(idx, RETURN_READ_BRBSRCN);
	return 0;
}

#define RETURN_READ_BRBTGTN(n) \
	return read_sysreg_s(SYS_BRBTGT_EL1(n))
static inline u64 get_brbtgt_reg(int idx)
{
	BRBE_REGN_SWITCH(idx, RETURN_READ_BRBTGTN);
	return 0;
}

#define RETURN_READ_BRBINFN(n) \
	return read_sysreg_s(SYS_BRBINF_EL1(n))
static inline u64 get_brbinf_reg(int idx)
{
	BRBE_REGN_SWITCH(idx, RETURN_READ_BRBINFN);
	return 0;
}

static inline u64 brbe_record_valid(u64 brbinf)
{
	return FIELD_GET(BRBINFx_EL1_VALID_MASK, brbinf);
}

static inline bool brbe_invalid(u64 brbinf)
{
	return brbe_record_valid(brbinf) == BRBINFx_EL1_VALID_NONE;
}

static inline bool brbe_record_is_complete(u64 brbinf)
{
	return brbe_record_valid(brbinf) == BRBINFx_EL1_VALID_FULL;
}

static inline bool brbe_record_is_source_only(u64 brbinf)
{
	return brbe_record_valid(brbinf) == BRBINFx_EL1_VALID_SOURCE;
}

static inline bool brbe_record_is_target_only(u64 brbinf)
{
	return brbe_record_valid(brbinf) == BRBINFx_EL1_VALID_TARGET;
}

static inline int brbinf_get_in_tx(u64 brbinf)
{
	return FIELD_GET(BRBINFx_EL1_T_MASK, brbinf);
}

static inline int brbinf_get_mispredict(u64 brbinf)
{
	return FIELD_GET(BRBINFx_EL1_MPRED_MASK, brbinf);
}

static inline int brbinf_get_lastfailed(u64 brbinf)
{
	return FIELD_GET(BRBINFx_EL1_LASTFAILED_MASK, brbinf);
}

static inline int brbinf_get_cycles(u64 brbinf)
{
	/*
	 * Captured cycle count is unknown and hence
	 * should not be passed on to userspace.
	 */
	if (brbinf & BRBINFx_EL1_CCU)
		return 0;

	return FIELD_GET(BRBINFx_EL1_CC_MASK, brbinf);
}

static inline int brbinf_get_type(u64 brbinf)
{
	return FIELD_GET(BRBINFx_EL1_TYPE_MASK, brbinf);
}

static inline int brbinf_get_el(u64 brbinf)
{
	return FIELD_GET(BRBINFx_EL1_EL_MASK, brbinf);
}

static inline int brbidr_get_numrec(u64 brbidr)
{
	return FIELD_GET(BRBIDR0_EL1_NUMREC_MASK, brbidr);
}

static inline int brbidr_get_format(u64 brbidr)
{
	return FIELD_GET(BRBIDR0_EL1_FORMAT_MASK, brbidr);
}

static inline int brbidr_get_cc_bits(u64 brbidr)
{
	return FIELD_GET(BRBIDR0_EL1_CC_MASK, brbidr);
}

void brbe_invalidate(void)
{
	asm volatile(BRB_IALL_INSN);
	isb();
}

static bool valid_brbe_nr(int brbe_nr)
{
	return brbe_nr == BRBIDR0_EL1_NUMREC_8 ||
	       brbe_nr == BRBIDR0_EL1_NUMREC_16 ||
	       brbe_nr == BRBIDR0_EL1_NUMREC_32 ||
	       brbe_nr == BRBIDR0_EL1_NUMREC_64;
}

static bool valid_brbe_cc(int brbe_cc)
{
	return brbe_cc == BRBIDR0_EL1_CC_20_BIT;
}

static bool valid_brbe_format(int brbe_format)
{
	return brbe_format == BRBIDR0_EL1_FORMAT_FORMAT_0;
}

static bool valid_brbe_version(int brbe_version)
{
	return brbe_version == ID_AA64DFR0_EL1_BRBE_IMP ||
	       brbe_version == ID_AA64DFR0_EL1_BRBE_BRBE_V1P1;
}

static void select_brbe_bank(int bank)
{
	u64 brbfcr;

	WARN_ON(bank > 1);
	brbfcr = read_sysreg_s(SYS_BRBFCR_EL1);
	brbfcr &= ~BRBFCR_EL1_BANK_MASK;
	brbfcr |= SYS_FIELD_PREP(BRBFCR_EL1, BANK, bank);
	write_sysreg_s(brbfcr, SYS_BRBFCR_EL1);
	isb();
}

static bool __read_brbe_regset(struct brbe_regset *entry, int idx)
{
	entry->brbinf = get_brbinf_reg(idx);

	if (brbe_invalid(entry->brbinf))
		return false;

	entry->brbsrc = get_brbsrc_reg(idx);
	entry->brbtgt = get_brbtgt_reg(idx);
	return true;
}

/*
 * Generic perf branch filters supported on BRBE
 *
 * New branch filters need to be evaluated whether they could be supported on
 * BRBE. This ensures that such branch filters would not just be accepted, to
 * fail silently. PERF_SAMPLE_BRANCH_HV is a special case that is selectively
 * supported only on platforms where kernel is in hyp mode.
 */
#define BRBE_EXCLUDE_BRANCH_FILTERS (PERF_SAMPLE_BRANCH_ABORT_TX	| \
				     PERF_SAMPLE_BRANCH_IN_TX		| \
				     PERF_SAMPLE_BRANCH_NO_TX		| \
				     PERF_SAMPLE_BRANCH_CALL_STACK	| \
				     PERF_SAMPLE_BRANCH_COUNTERS)

#define BRBE_ALLOWED_BRANCH_FILTERS (PERF_SAMPLE_BRANCH_USER		| \
				     PERF_SAMPLE_BRANCH_KERNEL		| \
				     PERF_SAMPLE_BRANCH_HV		| \
				     PERF_SAMPLE_BRANCH_ANY		| \
				     PERF_SAMPLE_BRANCH_ANY_CALL	| \
				     PERF_SAMPLE_BRANCH_ANY_RETURN	| \
				     PERF_SAMPLE_BRANCH_IND_CALL	| \
				     PERF_SAMPLE_BRANCH_COND		| \
				     PERF_SAMPLE_BRANCH_IND_JUMP	| \
				     PERF_SAMPLE_BRANCH_CALL		| \
				     PERF_SAMPLE_BRANCH_NO_FLAGS	| \
				     PERF_SAMPLE_BRANCH_NO_CYCLES	| \
				     PERF_SAMPLE_BRANCH_TYPE_SAVE	| \
				     PERF_SAMPLE_BRANCH_HW_INDEX	| \
				     PERF_SAMPLE_BRANCH_PRIV_SAVE)

#define BRBE_PERF_BRANCH_FILTERS    (BRBE_ALLOWED_BRANCH_FILTERS	| \
				     BRBE_EXCLUDE_BRANCH_FILTERS)

bool brbe_branch_attr_valid(struct perf_event *event)
{
	u64 branch_type = event->attr.branch_sample_type;

	/*
	 * Ensure both perf branch filter allowed and exclude
	 * masks are always in sync with the generic perf ABI.
	 */
	BUILD_BUG_ON(BRBE_PERF_BRANCH_FILTERS != (PERF_SAMPLE_BRANCH_MAX - 1));

	if (branch_type & ~BRBE_ALLOWED_BRANCH_FILTERS) {
		pr_debug_once("requested branch filter not supported 0x%llx\n", branch_type);
		return false;
	}

	/*
	 * If the event does not have at least one of the privilege
	 * branch filters as in PERF_SAMPLE_BRANCH_PLM_ALL, the core
	 * perf will adjust its value based on perf event's existing
	 * privilege level via attr.exclude_[user|kernel|hv].
	 *
	 * As event->attr.branch_sample_type might have been changed
	 * when the event reaches here, it is not possible to figure
	 * out whether the event originally had HV privilege request
	 * or got added via the core perf. Just report this situation
	 * once and continue ignoring if there are other instances.
	 */
	if ((branch_type & PERF_SAMPLE_BRANCH_HV) && !is_kernel_in_hyp_mode())
		pr_debug_once("hypervisor privilege filter not supported 0x%llx\n", branch_type);

	return true;
}

static int brbe_attributes_probe(struct arm_pmu *armpmu, u32 brbe)
{
	u64 brbidr = read_sysreg_s(SYS_BRBIDR0_EL1);
	int brbe_version, brbe_format, brbe_cc, brbe_nr;

	brbe_version = brbe;
	brbe_format = brbidr_get_format(brbidr);
	brbe_cc = brbidr_get_cc_bits(brbidr);
	brbe_nr = brbidr_get_numrec(brbidr);
	armpmu->reg_brbidr = brbidr;

	if (!valid_brbe_version(brbe_version) ||
	    !valid_brbe_format(brbe_format) ||
	    !valid_brbe_cc(brbe_cc) ||
	    !valid_brbe_nr(brbe_nr))
		return -EOPNOTSUPP;
	return 0;
}

void brbe_probe(struct arm_pmu *armpmu)
{
	u64 aa64dfr0 = read_sysreg_s(SYS_ID_AA64DFR0_EL1);
	u32 brbe;

	brbe = cpuid_feature_extract_unsigned_field(aa64dfr0, ID_AA64DFR0_EL1_BRBE_SHIFT);
	if (!brbe)
		return;

	if (brbe_attributes_probe(armpmu, brbe))
		return;

	armpmu->num_branch_records = brbidr_get_numrec(armpmu->reg_brbidr);
}

/*
 * BRBE supports the following functional branch type filters while
 * generating branch records. These branch filters can be enabled,
 * either individually or as a group i.e ORing multiple filters
 * with each other.
 *
 * BRBFCR_EL1_CONDDIR  - Conditional direct branch
 * BRBFCR_EL1_DIRCALL  - Direct call
 * BRBFCR_EL1_INDCALL  - Indirect call
 * BRBFCR_EL1_INDIRECT - Indirect branch
 * BRBFCR_EL1_DIRECT   - Direct branch
 * BRBFCR_EL1_RTN      - Subroutine return
 */
static u64 branch_type_to_brbfcr(int branch_type)
{
	u64 brbfcr = 0;

	if (branch_type & PERF_SAMPLE_BRANCH_ANY) {
		brbfcr |= BRBFCR_EL1_BRANCH_FILTERS;
		return brbfcr;
	}

	if (branch_type & PERF_SAMPLE_BRANCH_ANY_CALL) {
		brbfcr |= BRBFCR_EL1_INDCALL;
		brbfcr |= BRBFCR_EL1_DIRCALL;
	}

	if (branch_type & PERF_SAMPLE_BRANCH_ANY_RETURN)
		brbfcr |= BRBFCR_EL1_RTN;

	if (branch_type & PERF_SAMPLE_BRANCH_IND_CALL)
		brbfcr |= BRBFCR_EL1_INDCALL;

	if (branch_type & PERF_SAMPLE_BRANCH_COND)
		brbfcr |= BRBFCR_EL1_CONDDIR;

	if (branch_type & PERF_SAMPLE_BRANCH_IND_JUMP)
		brbfcr |= BRBFCR_EL1_INDIRECT;

	if (branch_type & PERF_SAMPLE_BRANCH_CALL)
		brbfcr |= BRBFCR_EL1_DIRCALL;

	return brbfcr & BRBFCR_EL1_CONFIG_MASK;
}

/*
 * BRBE supports the following privilege mode filters while generating
 * branch records.
 *
 * BRBCR_ELx_E0BRE - EL0 branch records
 * BRBCR_ELx_ExBRE - EL1/EL2 branch records
 *
 * BRBE also supports the following additional functional branch type
 * filters while generating branch records.
 *
 * BRBCR_ELx_EXCEPTION - Exception
 * BRBCR_ELx_ERTN     -  Exception return
 */
static u64 branch_type_to_brbcr(int branch_type)
{
	u64 brbcr = BRBCR_ELx_DEFAULT_TS;

	/*
	 * BRBE should be paused on PMU interrupt while tracing kernel
	 * space to stop capturing further branch records. Otherwise
	 * interrupt handler branch records might get into the samples
	 * which is not desired.
	 *
	 * BRBE need not be paused on PMU interrupt while tracing only
	 * the user space, because it will automatically be inside the
	 * prohibited region. But even after PMU overflow occurs, the
	 * interrupt could still take much more cycles, before it can
	 * be taken and by that time BRBE will have been overwritten.
	 * Hence enable pause on PMU interrupt mechanism even for user
	 * only traces as well.
	 */
	brbcr |= BRBCR_ELx_FZP;

	if (branch_type & PERF_SAMPLE_BRANCH_USER)
		brbcr |= BRBCR_ELx_E0BRE;

	/*
	 * When running in the hyp mode, writing into BRBCR_EL1
	 * actually writes into BRBCR_EL2 instead. Field E2BRE
	 * is also at the same position as E1BRE.
	 */
	if (branch_type & PERF_SAMPLE_BRANCH_KERNEL)
		brbcr |= BRBCR_ELx_ExBRE;

	if (branch_type & PERF_SAMPLE_BRANCH_HV) {
		if (is_kernel_in_hyp_mode())
			brbcr |= BRBCR_ELx_ExBRE;
	}

	if (!(branch_type & PERF_SAMPLE_BRANCH_NO_CYCLES))
		brbcr |= BRBCR_ELx_CC;

	if (!(branch_type & PERF_SAMPLE_BRANCH_NO_FLAGS))
		brbcr |= BRBCR_ELx_MPRED;

	/*
	 * The exception and exception return branches could be
	 * captured, irrespective of the perf event's privilege.
	 * If the perf event does not have enough privilege for
	 * a given exception level, then addresses which falls
	 * under that exception level will be reported as zero
	 * for the captured branch record, creating source only
	 * or target only records.
	 */
	if (branch_type & PERF_SAMPLE_BRANCH_ANY) {
		brbcr |= BRBCR_ELx_EXCEPTION;
		brbcr |= BRBCR_ELx_ERTN;
	}

	if (branch_type & PERF_SAMPLE_BRANCH_ANY_CALL)
		brbcr |= BRBCR_ELx_EXCEPTION;

	if (branch_type & PERF_SAMPLE_BRANCH_ANY_RETURN)
		brbcr |= BRBCR_ELx_ERTN;

	return brbcr & BRBCR_ELx_CONFIG_MASK;
}

void brbe_enable(struct arm_pmu *arm_pmu)
{
	struct pmu_hw_events *cpuc = this_cpu_ptr(arm_pmu->hw_events);
	u64 brbfcr, brbcr;
	u64 sample_type;

	/*
	 * Merge the permitted branch filters of all events.
	 */
	for (int i = 0; i < ARMPMU_MAX_HWEVENTS; i++) {
		struct perf_event *event = cpuc->events[i];
		if (event && has_branch_stack(event))
			sample_type |= event->attr.branch_sample_type;
	}

	/*
	 * TODO: explain why we disacrd any prior branches
	 * TODO: avoid the redundant ISB?
	 */
	brbe_invalidate();

	/*
	 * BRBE gets configured with a new mismatched branch sample
	 * type request, overriding any previous branch filters.
	 */
	brbfcr = read_sysreg_s(SYS_BRBFCR_EL1);
	brbfcr &= ~BRBFCR_EL1_CONFIG_MASK;
	brbfcr |= branch_type_to_brbfcr(sample_type);
	write_sysreg_s(brbfcr, SYS_BRBFCR_EL1);
	isb();

	brbcr = read_sysreg_s(SYS_BRBCR_EL1);
	brbcr &= ~BRBCR_ELx_CONFIG_MASK;
	brbcr |= branch_type_to_brbcr(sample_type);
	write_sysreg_s(brbcr, SYS_BRBCR_EL1);
	isb();
}

void brbe_disable(struct arm_pmu *arm_pmu)
{
	u64 brbfcr, brbcr;

	/*
	 * TODO: We shouldn't need to configure *both* registers to disable
	 * recording.
	 */

	brbcr = read_sysreg_s(SYS_BRBCR_EL1);
	brbfcr = read_sysreg_s(SYS_BRBFCR_EL1);
	brbcr &= ~(BRBCR_ELx_E0BRE | BRBCR_ELx_ExBRE);
	brbfcr |= BRBFCR_EL1_PAUSED;
	write_sysreg_s(brbcr, SYS_BRBCR_EL1);
	write_sysreg_s(brbfcr, SYS_BRBFCR_EL1);
	isb();
}

static const int brbe_type_to_perf_type_map[BRBINFx_EL1_TYPE_DEBUG_EXIT + 1][2] = {
	[BRBINFx_EL1_TYPE_DIRECT_UNCOND] = { PERF_BR_UNCOND, 0 },
	[BRBINFx_EL1_TYPE_INDIRECT] = { PERF_BR_IND, 0 },
	[BRBINFx_EL1_TYPE_DIRECT_LINK] = { PERF_BR_CALL, 0 },
	[BRBINFx_EL1_TYPE_INDIRECT_LINK] = { PERF_BR_IND_CALL, 0 },
	[BRBINFx_EL1_TYPE_RET] = { PERF_BR_RET, 0 },
	[BRBINFx_EL1_TYPE_DIRECT_COND] = { PERF_BR_COND, 0 },
	[BRBINFx_EL1_TYPE_CALL] = { PERF_BR_CALL, 0 },
	[BRBINFx_EL1_TYPE_TRAP] = { PERF_BR_SYSCALL, 0 },
	[BRBINFx_EL1_TYPE_ERET] = { PERF_BR_ERET, 0 },
	[BRBINFx_EL1_TYPE_IRQ] = { PERF_BR_IRQ, 0 },
	[BRBINFx_EL1_TYPE_SERROR] = { PERF_BR_SERROR, 0 },
	[BRBINFx_EL1_TYPE_DEBUG_HALT] = { PERF_BR_EXTEND_ABI, PERF_BR_ARM64_DEBUG_HALT },
	[BRBINFx_EL1_TYPE_INSN_DEBUG] = { PERF_BR_EXTEND_ABI, PERF_BR_ARM64_DEBUG_INST },
	[BRBINFx_EL1_TYPE_DATA_DEBUG] = { PERF_BR_EXTEND_ABI, PERF_BR_ARM64_DEBUG_DATA },
	[BRBINFx_EL1_TYPE_ALIGN_FAULT] = { PERF_BR_EXTEND_ABI, PERF_BR_NEW_FAULT_ALGN },
	[BRBINFx_EL1_TYPE_INSN_FAULT] = { PERF_BR_EXTEND_ABI, PERF_BR_NEW_FAULT_INST },
	[BRBINFx_EL1_TYPE_DATA_FAULT] = { PERF_BR_EXTEND_ABI, PERF_BR_ARM64_DEBUG_HALT },
	[BRBINFx_EL1_TYPE_FIQ] = { PERF_BR_EXTEND_ABI, PERF_BR_ARM64_FIQ },
	[BRBINFx_EL1_TYPE_DEBUG_EXIT] = { PERF_BR_EXTEND_ABI, PERF_BR_ARM64_DEBUG_EXIT },
};

static void brbe_set_perf_entry_type(struct perf_branch_entry *entry, u64 brbinf)
{
	int brbe_type = brbinf_get_type(brbinf);

	if (brbe_type <= BRBINFx_EL1_TYPE_DEBUG_EXIT) {
		const int *br_type = brbe_type_to_perf_type_map[brbinf_get_type(brbinf)];
		entry->type = br_type[0];
		entry->new_type = br_type[1];
	}
	if (!entry->type)
		pr_warn_once("%d - unknown branch type captured\n", brbe_type);
}

static int brbinf_get_perf_priv(u64 brbinf)
{
	int brbe_el = brbinf_get_el(brbinf);

	switch (brbe_el) {
	case BRBINFx_EL1_EL_EL0:
		return PERF_BR_PRIV_USER;
	case BRBINFx_EL1_EL_EL1:
		return PERF_BR_PRIV_KERNEL;
	case BRBINFx_EL1_EL_EL2:
		if (is_kernel_in_hyp_mode())
			return PERF_BR_PRIV_KERNEL;
		return PERF_BR_PRIV_HV;
	default:
		pr_warn_once("%d - unknown branch privilege captured\n", brbe_el);
		return PERF_BR_PRIV_UNKNOWN;
	}
}

static void capture_brbe_flags(struct perf_branch_entry *entry, struct perf_event *event,
			       u64 brbinf)
{
	brbe_set_perf_entry_type(entry, brbinf);

	if (!branch_sample_no_cycles(event))
		entry->cycles = brbinf_get_cycles(brbinf);

	if (!branch_sample_no_flags(event)) {
		/*
		 * BRBINFx_EL1.LASTFAILED indicates that a TME transaction failed (or
		 * was cancelled) prior to this record, and some number of records
		 * prior to this one, may have been generated during an attempt to
		 * execute the transaction.
		 */
		entry->abort = brbinf_get_lastfailed(brbinf);

		/*
		 * All these information (i.e transaction state and mispredicts)
		 * are available for source only and complete branch records.
		 */
		if (brbe_record_is_complete(brbinf) ||
		    brbe_record_is_source_only(brbinf)) {
			entry->mispred = brbinf_get_mispredict(brbinf);
			entry->predicted = !entry->mispred;
			entry->in_tx = brbinf_get_in_tx(brbinf);
		}

		/*
		 * Currently TME feature is neither implemented in any hardware
		 * nor it is being supported in the kernel. Just warn here once
		 * if TME related information shows up rather unexpectedly.
		 */
		if (entry->abort || entry->in_tx)
			pr_warn_once("Unknown transaction states %d %d\n",
				      entry->abort, entry->in_tx);
	}

	/*
	 * All these information (i.e branch privilege level) are
	 * available for target only and complete branch records.
	 */
	if (brbe_record_is_complete(brbinf) ||
	    brbe_record_is_target_only(brbinf))
		entry->priv = brbinf_get_perf_priv(brbinf);
}

static void perf_entry_from_brbe_regset(struct perf_branch_entry *entry,
					struct brbe_regset *regset,
					struct perf_event *event)
{
	perf_clear_branch_entry_bitfields(entry);
	if (brbe_record_is_complete(regset->brbinf)) {
		entry->from = regset->brbsrc;
		entry->to = regset->brbtgt;
	} else if (brbe_record_is_source_only(regset->brbinf)) {
		entry->from = regset->brbsrc;
		entry->to = 0;
	} else if (brbe_record_is_target_only(regset->brbinf)) {
		entry->from = 0;
		entry->to = regset->brbtgt;
	}
	capture_brbe_flags(entry, event, regset->brbinf);
}

static bool filter_branch_privilege(struct perf_branch_entry *entry, u64 branch_sample_type)
{
	/*
	 * Retrieve the privilege level branch filter requests
	 * from the overall branch sample type.
	 */
	branch_sample_type &= PERF_SAMPLE_BRANCH_PLM_ALL;

	/*
	 * The privilege information do not always get captured
	 * successfully for given BRBE branch record. Hence the
	 * entry->priv could be analyzed for filtering when the
	 * information has really been captured.
	 */
	if (entry->priv) {
		if (entry->priv == PERF_BR_PRIV_USER) {
			if (!(branch_sample_type & PERF_SAMPLE_BRANCH_USER))
				return false;
		}

		if (entry->priv == PERF_BR_PRIV_KERNEL) {
			if (!(branch_sample_type & PERF_SAMPLE_BRANCH_KERNEL)) {
				if (!is_kernel_in_hyp_mode())
					return false;

				if (!(branch_sample_type & PERF_SAMPLE_BRANCH_HV))
					return false;
			}
		}

		if (entry->priv == PERF_BR_PRIV_HV) {
			/*
			 * PERF_SAMPLE_BRANCH_HV request actually gets configured
			 * similar to PERF_SAMPLE_BRANCH_KERNEL when kernel is in
			 * hyp mode. In that case PERF_BR_PRIV_KERNEL should have
			 * been reported for corresponding branch records.
			 */
			pr_warn_once("PERF_BR_PRIV_HV should not have been captured\n");
		}
		return true;
	}

	if (is_ttbr0_addr(entry->from) || is_ttbr0_addr(entry->to)) {
		if (!(branch_sample_type & PERF_SAMPLE_BRANCH_USER))
			return false;
	}

	if (is_ttbr1_addr(entry->from) || is_ttbr1_addr(entry->to)) {
		if (!(branch_sample_type & PERF_SAMPLE_BRANCH_KERNEL)) {
			if (!is_kernel_in_hyp_mode())
				return false;

			if (!(branch_sample_type & PERF_SAMPLE_BRANCH_HV))
				return false;
		}
	}
	return true;
}

static bool filter_branch_record(struct perf_event *event,
				 struct perf_branch_entry *entry)
{
	u64 branch_sample = event->attr.branch_sample_type;
	DECLARE_BITMAP(entry_type_mask, PERF_BR_ARM64_MAX);
	DECLARE_BITMAP(event_type_mask, PERF_BR_ARM64_MAX);

	if (!filter_branch_privilege(entry, branch_sample))
		return false;

	if (entry->type == PERF_BR_UNKNOWN)
		return true;

	if (branch_sample & PERF_SAMPLE_BRANCH_ANY)
		return true;

	branch_entry_mask(entry, entry_type_mask);

	prepare_event_branch_type_mask(event, event_type_mask);
	return bitmap_subset(entry_type_mask, event_type_mask, PERF_BR_ARM64_MAX);
}

void brbe_read_filtered_entries(struct perf_branch_stack *branch_stack, struct perf_event *event)
{
	struct arm_pmu *cpu_pmu = to_arm_pmu(event->pmu);
	int nr_hw = brbidr_get_numrec(cpu_pmu->reg_brbidr);
	int nr_banks = DIV_ROUND_UP(nr_hw, BRBE_BANK_MAX_ENTRIES);
	int nr_filtered = 0;

	for (int bank = 0; bank < nr_banks; bank++) {
		int nr_remaining = nr_hw - (bank * BRBE_BANK_MAX_ENTRIES);
		int nr_this_bank = min(nr_remaining, BRBE_BANK_MAX_ENTRIES);

		select_brbe_bank(bank);

		for (int i = 0; i < nr_this_bank; i++) {
			struct perf_branch_entry pbe;
			struct brbe_regset bregs;

			if (!__read_brbe_regset(&bregs, i))
				goto done;

			perf_entry_from_brbe_regset(&pbe, &bregs, event);
			if (!filter_branch_record(event, &pbe))
				continue;

			branch_stack->entries[nr_filtered] = pbe;
			nr_filtered++;
		}
	}

done:
	branch_stack->nr = nr_filtered;
}
