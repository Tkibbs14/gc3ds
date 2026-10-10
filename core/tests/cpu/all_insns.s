# One of every instruction form the interpreter implements that the fuzzer does not generate (Gekko-only,
# privileged, traps, estimates). Assembled with powerpc-eabi-as -mgekko and used by run_tests.sh to cross-check
# gekko_disasm against objdump -M gekko,raw.
	.text
	ps_add 1,2,3
	ps_add. 1,2,3
	ps_sub 1,2,3
	ps_mul 1,2,3
	ps_div 1,2,3
	ps_madd 1,2,3,4
	ps_msub 1,2,3,4
	ps_nmadd 1,2,3,4
	ps_nmsub 1,2,3,4
	ps_madds0 1,2,3,4
	ps_madds1 1,2,3,4
	ps_muls0 1,2,3
	ps_muls1 1,2,3
	ps_sum0 1,2,3,4
	ps_sum1 1,2,3,4
	ps_sel 1,2,3,4
	ps_res 1,2
	ps_rsqrte 1,2
	ps_neg 1,2
	ps_abs 1,2
	ps_nabs 1,2
	ps_mr 1,2
	ps_merge00 1,2,3
	ps_merge01 1,2,3
	ps_merge10 1,2,3
	ps_merge11 1,2,3
	ps_cmpu0 1,2,3
	ps_cmpo0 2,2,3
	ps_cmpu1 3,2,3
	ps_cmpo1 7,2,3
	psq_l 1,-8(3),0,2
	psq_lu 1,2047(3),1,7
	psq_st 1,-2048(3),0,0
	psq_stu 1,4(3),1,5
	psq_lx 1,3,4,0,1
	psq_lux 1,3,4,1,6
	psq_stx 1,3,4,0,3
	psq_stux 1,3,4,1,4
	dcbz_l 3,4
	fres 1,2
	fres. 1,2
	frsqrte 1,2
	frsqrte. 1,2
	tw 4,3,4
	twi 31,3,-5
	sc
	rfi
	mfmsr 3
	mtmsr 3
	mfsr 3,7
	mtsr 7,3
	mfsrin 3,4
	mtsrin 3,4
	tlbie 4
	tlbsync
	dcbi 3,4
	mfspr 3,912
	mtspr 920,3
	mftb 3,268
	mftb 3,269
	eciwx 3,4,5
	ecowx 3,4,5
	mcrxr 3
	lmw 20,8(3)
	stmw 20,8(3)
