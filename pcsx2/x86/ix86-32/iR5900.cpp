// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "Common.h"
#include "CDVD/CDVD.h"
#include "DebugTools/Breakpoints.h"
#include "Elfheader.h"
#include "GS.h"
#include "Host.h"
#include "Memory.h"
#include "Patch.h"
#include "R3000A.h"
#include "R5900OpcodeTables.h"
#include "VMManager.h"
#include "vtlb.h"
#include "x86/BaseblockEx.h"
#include "x86/iR5900.h"
#include "x86/iR5900Analysis.h"

#include "common/AlignedMalloc.h"
#include "common/FastJmp.h"
#include "common/HeapArray.h"
#include "common/Perf.h"

#include "common/emitter/internal.h"

#ifdef DUMP_BLOCKS
#include "Zydis/Zydis.h"
#include "Zycore/Format.h"
#include "Zycore/Status.h"
#endif

#ifdef TRACE_BLOCKS
#include <zlib.h>
#endif

using namespace x86Emitter;
using namespace R5900;

static bool eeRecNeedsReset = false;
static bool eeCpuExecuting = false;
static bool eeRecExitRequested = false;
static bool g_resetEeScalingStats = false;

#define PC_GETBLOCK(x) PC_GETBLOCK_(x, recLUT)

u32 maxrecmem = 0;
alignas(16) static uptr recLUT[_64kb];
alignas(16) static u32 hwLUT[_64kb];

static __fi u32 HWADDR(u32 mem) { return hwLUT[mem >> 16] + mem; }

u32 s_nBlockCycles = 0;
bool s_nBlockInterlocked = false;
u32 pc;
int g_branch;

alignas(16) GPR_reg64 g_cpuConstRegs[32] = {};
u32 g_cpuHasConstReg = 0, g_cpuFlushedConstReg = 0;
bool g_cpuFlushedPC, g_cpuFlushedCode, g_recompilingDelaySlot, g_maySignalException;

eeProfiler EE::Profiler;

#define X86

static DynamicHeapArray<u8, 4096> recRAMCopy;
static DynamicHeapArray<BASEBLOCK, 4096> recLutReserve_RAM;
static DynamicHeapArray<BASEBLOCK, 4096> recLutUnmapped;
static size_t recLutEntries;
static bool extraRam;

static BASEBLOCK* recRAM = nullptr;
static BASEBLOCK* recROM = nullptr;
static BASEBLOCK* recROM1 = nullptr;
static BASEBLOCK* recROM2 = nullptr;

static BaseBlocks recBlocks;
static u8* recPtr = nullptr;
static u8* recPtrEnd = nullptr;
EEINST* s_pInstCache = nullptr;
static u32 s_nInstCacheSize = 0;

static BASEBLOCK* s_pCurBlock = nullptr;
static BASEBLOCKEX* s_pCurBlockEx = nullptr;
u32 s_nEndBlock = 0;
u32 s_branchTo;
static bool s_nBlockFF;

GPR_reg64 s_saveConstRegs[32];
static u32 s_saveHasConstReg = 0, s_saveFlushedConstReg = 0;
static EEINST* s_psaveInstInfo = nullptr;

static u32 s_savenBlockCycles = 0;

static void iBranchTest(u32 newpc = 0xffffffff);
static void ClearRecLUT(BASEBLOCK* base, int count);
static u32 scaleblockcycles();
static void recExitExecution();

#ifdef TRACE_BLOCKS
static void pauseAAA()
{
	fprintf(stderr, "\nPaused\n");
	fflush(stdout);
	fflush(stderr);
#ifdef _MSC_VER
	__debugbreak();
#else
	sleep(1);
#endif
}
#endif

#ifdef DUMP_BLOCKS
static ZydisFormatterFunc s_old_print_address;
static ZydisFormatterFunc s_old_print_disp;

static bool Address2Symbol(u64 address, SmallString &buf)
{

#define A(x) ((u64)(x))

	if (address >= A(eeMem->Main) && address < A(eeMem->Scratch))
	{
		buf.append_format("eeMem+0x{:08X}", static_cast<u32>(address - A(eeMem->Main)));
	}
	else if (address >= A(eeMem->Scratch) && address < A(eeMem->ROM))
	{

		buf.append_format("eeScratchpad+0x{:08X}", static_cast<u32>(address - A(eeMem->Scratch)));
	}
	else if (address >= A(&cpuRegs.GPR) && address < A(&cpuRegs.HI))
	{
		const u32 offset = static_cast<u32>(address - A(&cpuRegs)) % 16u;
		if (offset != 0)
			buf.append_format("cpuRegs.GPR.{}+{}", GPR_REG[static_cast<u32>(address - A(&cpuRegs)) / 16u], offset);
		else
			buf.append_format("cpuRegs.GPR.{}", GPR_REG[static_cast<u32>(address - A(&cpuRegs)) / 16u]);
	}
	else if (address >= A(&cpuRegs.HI) && address < A(&cpuRegs.CP0))
	{
		const u32 offset = static_cast<u32>(address - A(&cpuRegs.HI)) % 16u;
		if (offset != 0)
			buf.append_format("cpuRegs.{}+{}", (address >= A(&cpuRegs.LO) ? "LO" : "HI"), offset);
		else
			buf.append_format("cpuRegs.{}", (address >= A(&cpuRegs.LO) ? "LO" : "HI"));
	}
	else if (address == A(&cpuRegs.pc))
	{
		buf.append_format("cpuRegs.pc");
	}
	else if (address == A(&cpuRegs.cycle))
	{
		buf.append_format("cpuRegs.cycle");
	}
	else if (address == A(&cpuRegs.nextEventCycle))
	{
		buf.append_format("cpuRegs.nextEventCycle");
	}
	else if (address >= A(fpuRegs.fpr) && address < A(fpuRegs.fprc))
	{
		buf.append_format("fpuRegs.f{:02}", static_cast<u32>(address - A(fpuRegs.fpr)) / 4u);
	}
	else if (address >= A(&VU0.VF[0]) && address < A(&VU0.VI[0]))
	{
		const u32 offset = static_cast<u32>(address - A(&VU0.VF[0])) % 16u;
		if (offset != 0)
			buf.append_format("VU0.VF[{:02}]+{}", static_cast<u32>(address - A(&VU0.VF[0])) / 16u, offset);
		else
			buf.append_format("VU0.VF[{:02}]", static_cast<u32>(address - A(&VU0.VF[0])) / 16u);
	}
	else if (address >= A(&VU0.VI[0]) && address < A(&VU0.ACC))
	{
		const u32 offset = static_cast<u32>(address - A(&VU0.VI[0])) % 16u;
		const u32 vi = static_cast<u32>(address - A(&VU0.VI[0])) / 16u;
		if (offset != 0)
			buf.append_format("VU0.{}+{}", COP2_REG_CTL[vi], offset);
		else
			buf.append_format("VU0.{}", COP2_REG_CTL[vi]);
	}
	else if (address >= A(&VU0.ACC) && address < A(&VU0.q))
	{
		const u32 offset = static_cast<u32>(address - A(&VU0.ACC));
		if (offset != 0)
			buf.append_format("VU0.ACC+{}", offset);
		else
			buf.append("VU0.ACC");
	}
	else if (address >= A(&VU0.q) && address < A(&VU0.idx))
	{
		const u32 offset = static_cast<u32>(address - A(&VU0.q)) % 16u;
		const char* reg = (address >= A(&VU0.p)) ? "p" : "q";
		if (offset != 0)
			buf.append_format("VU0.{}+{}", reg, offset);
		else
			buf.append_format("VU0.{}", reg);
	}
	else
	{
		return false;
	}

#undef A

	return true;
}

static ZyanStatus ZydisFormatterPrintDisplacement(const ZydisFormatter* formatter,
	ZydisFormatterBuffer* buffer, ZydisFormatterContext* context)
{
	u64 address = (u64)R5900_TEXTPTR + context->operand->mem.disp.value;
	bool did_print = false;
	SmallString buf;

	if (context->operand->mem.base == ZYDIS_REGISTER_RBX) {
		did_print = Address2Symbol(address, buf);
	}

	if (did_print)
	{
		ZYAN_CHECK(ZydisFormatterBufferAppend(buffer, ZYDIS_TOKEN_SYMBOL));
		ZyanString* string;
		ZYAN_CHECK(ZydisFormatterBufferGetString(buffer, &string));
		return ZyanStringAppendFormat(string, "-%s+&%s", "R5900_TEXTPTR", buf.c_str());
	}

	return s_old_print_disp(formatter, buffer, context);
}

static ZyanStatus ZydisFormatterPrintAddressAbsolute(const ZydisFormatter* formatter,
	ZydisFormatterBuffer* buffer, ZydisFormatterContext* context)
{
	ZyanU64 address;
	ZYAN_CHECK(ZydisCalcAbsoluteAddress(context->instruction, context->operand,
		context->runtime_address, &address));

	bool did_print = false;
	SmallString buf;

	did_print = Address2Symbol(address, buf);

	if (did_print)
	{
		ZYAN_CHECK(ZydisFormatterBufferAppend(buffer, ZYDIS_TOKEN_SYMBOL));
		ZyanString* string;
		ZYAN_CHECK(ZydisFormatterBufferGetString(buffer, &string));
		return ZyanStringAppendFormat(string, "&%s", buf.c_str());
	}

	return s_old_print_address(formatter, buffer, context);
}
#endif

void _eeFlushAllDirty()
{
	_flushXMMregs();
	_flushX86regs();

	_flushConstRegs(false);
}

void _eeMoveGPRtoR(const xRegister32& to, int fromgpr, bool allow_preload)
{
	if (fromgpr == 0)
		xXOR(to, to);
	else if (GPR_IS_CONST1(fromgpr))
		xMOV(to, g_cpuConstRegs[fromgpr].UL[0]);
	else
	{
		int x86reg = _checkX86reg(X86TYPE_GPR, fromgpr, MODE_READ);
		int xmmreg = _checkXMMreg(XMMTYPE_GPRREG, fromgpr, MODE_READ);

		if (allow_preload && x86reg < 0 && xmmreg < 0)
		{
			if (EEINST_XMMUSEDTEST(fromgpr))
				xmmreg = _allocGPRtoXMMreg(fromgpr, MODE_READ);
			else if (EEINST_USEDTEST(fromgpr))
				x86reg = _allocX86reg(X86TYPE_GPR, fromgpr, MODE_READ);
		}

		if (x86reg >= 0)
			xMOV(to, xRegister32(x86reg));
		else if (xmmreg >= 0)
			xMOVD(to, xRegisterSSE(xmmreg));
		else
			xMOV(to, ptr[&cpuRegs.GPR.r[fromgpr].UL[0]]);
	}
}

void _eeMoveGPRtoR(const xRegister64& to, int fromgpr, bool allow_preload)
{
	if (fromgpr == 0)
		xXOR(xRegister32(to), xRegister32(to));
	else if (GPR_IS_CONST1(fromgpr))
		xMOV64(to, g_cpuConstRegs[fromgpr].UD[0]);
	else
	{
		int x86reg = _checkX86reg(X86TYPE_GPR, fromgpr, MODE_READ);
		int xmmreg = _checkXMMreg(XMMTYPE_GPRREG, fromgpr, MODE_READ);

		if (allow_preload && x86reg < 0 && xmmreg < 0)
		{
			if (EEINST_XMMUSEDTEST(fromgpr))
				xmmreg = _allocGPRtoXMMreg(fromgpr, MODE_READ);
			else if (EEINST_USEDTEST(fromgpr))
				x86reg = _allocX86reg(X86TYPE_GPR, fromgpr, MODE_READ);
		}

		if (x86reg >= 0)
			xMOV(to, xRegister64(x86reg));
		else if (xmmreg >= 0)
			xMOVD(to, xRegisterSSE(xmmreg));
		else
			xMOV(to, ptr32[&cpuRegs.GPR.r[fromgpr].UD[0]]);
	}
}

void _eeMoveGPRtoM(uptr to, int fromgpr)
{
	if (GPR_IS_CONST1(fromgpr))
		xMOV(ptr32[(u32*)(to)], g_cpuConstRegs[fromgpr].UL[0]);
	else
	{
		int x86reg = _checkX86reg(X86TYPE_GPR, fromgpr, MODE_READ);
		int xmmreg = _checkXMMreg(XMMTYPE_GPRREG, fromgpr, MODE_READ);

		if (x86reg < 0 && xmmreg < 0)
		{
			if (EEINST_XMMUSEDTEST(fromgpr))
				xmmreg = _allocGPRtoXMMreg(fromgpr, MODE_READ);
			else if (EEINST_USEDTEST(fromgpr))
				x86reg = _allocX86reg(X86TYPE_GPR, fromgpr, MODE_READ);
		}

		if (x86reg >= 0)
		{
			xMOV(ptr32[(void*)(to)], xRegister32(x86reg));
		}
		else if (xmmreg >= 0)
		{
			xMOVSS(ptr32[(void*)(to)], xRegisterSSE(xmmreg));
		}
		else
		{
			xMOV(eax, ptr32[&cpuRegs.GPR.r[fromgpr].UL[0]]);
			xMOV(ptr32[(void*)(to)], eax);
		}
	}
}

void recBranchCall(void (*func)())
{

	xMOV(rax, ptr64[&cpuRegs.cycle]);
	xMOV(ptr64[&cpuRegs.nextEventCycle], rax);

	recCall(func);
	g_branch = 2;
}

void recCall(void (*func)())
{
	iFlushCall(FLUSH_INTERPRETER);
	xFastCall((void*)func);
}

static void recRecompile(const u32 startpc);
static void dyna_block_discard(u32 start, u32 sz);
static void dyna_page_reset(u32 start, u32 sz);
static void recError(u32 error);

static const void* DispatcherEvent = nullptr;
static const void* DispatcherReg = nullptr;
static const void* JITCompile = nullptr;
static const void* EnterRecompiledCode = nullptr;
static const void* DispatchBlockDiscard = nullptr;
static const void* DispatchPageReset = nullptr;
static const void* UnmappedRecLUTPage = nullptr;

static void recEventTest()
{
	_cpuEventTest_Shared();

	if (eeRecExitRequested)
	{
		eeRecExitRequested = false;
		recExitExecution();
	}
}

static const void* _DynGen_JITCompile()
{
	pxAssertMsg(DispatcherReg != NULL, "Please compile the DispatcherReg subroutine *before* JITComple.  Thanks.");

	u8* retval = xGetAlignedCallTarget();

	xFastCall((const void*)recRecompile, ptr32[&cpuRegs.pc]);

	xMOV(eax, ptr[&cpuRegs.pc]);
	xMOV(edx, eax);
	xSHR(eax, 16);
	xMOV(rcx, ptrNative[xComplexAddress(rcx, recLUT, rax * wordsize)]);
	xJMP(ptrNative[rdx * (wordsize / 4) + rcx]);

	return retval;
}

static const void* _DynGen_DispatcherReg()
{
	u8* retval = xGetPtr();

	xMOV(eax, ptr[&cpuRegs.pc]);
	xMOV(edx, eax);
	xSHR(eax, 16);
	xMOV(rcx, ptrNative[xComplexAddress(rcx, recLUT, rax * wordsize)]);
	xJMP(ptrNative[rdx * (wordsize / 4) + rcx]);

	return retval;
}

static const void* _DynGen_DispatcherEvent()
{
	u8* retval = xGetPtr();

	xFastCall((const void*)recEventTest);

	return retval;
}

static const void* _DynGen_EnterRecompiledCode()
{
	pxAssertMsg(DispatcherReg, "Dynamically generated dispatchers are required prior to generating EnterRecompiledCode!");

	u8* retval = xGetAlignedCallTarget();

#ifdef ENABLE_VTUNE
	xScopedStackFrame frame(true, true);
#else
#ifdef _WIN32
	static constexpr u32 stack_size = 32 + 8;
#else
	static constexpr u32 stack_size = 8;
#endif

	xSUB(rsp, stack_size);

	if (u8* ptr = xGetTextPtr())
		xLoadFarAddr(RTEXTPTR, ptr);
#endif

	if (CHECK_FASTMEM)
		xMOV(RFASTMEMBASE, ptrNative[&vtlb_private::vtlbdata.fastmem_base]);

	xJMP(DispatcherReg);

	return retval;
}

static const void* _DynGen_DispatchBlockDiscard()
{
	u8* retval = xGetPtr();
	xFastCall((const void*)dyna_block_discard);
	xJMP(DispatcherReg);
	return retval;
}

static const void* _DynGen_DispatchPageReset()
{
	u8* retval = xGetPtr();
	xFastCall((const void*)dyna_page_reset);
	xJMP(DispatcherReg);
	return retval;
}

static const void* _DynGen_UnmappedRecLUTPage()
{
	u8* retval = xGetPtr();
	xFastCall((const void*)recError, 0);
	return retval;
}

static void _DynGen_Dispatchers()
{
	const u8* start = xGetAlignedCallTarget();

	DispatcherEvent = _DynGen_DispatcherEvent();
	DispatcherReg = _DynGen_DispatcherReg();

	JITCompile = _DynGen_JITCompile();
	EnterRecompiledCode = _DynGen_EnterRecompiledCode();
	DispatchBlockDiscard = _DynGen_DispatchBlockDiscard();
	DispatchPageReset = _DynGen_DispatchPageReset();
	UnmappedRecLUTPage = _DynGen_UnmappedRecLUTPage();

	recBlocks.SetJITCompile(JITCompile);

	Perf::any.Register(start, static_cast<u32>(xGetPtr() - start), "EE Dispatcher");
}


static void recError(u32 error)
{
	switch (error)
	{
		case 0:
			Host::ReportErrorAsync("R5900 Exception", fmt::format("Jump to unmapped recLUT page (PC: 0x{:08x})", cpuRegs.pc));
			break;
		case 1:
			Host::ReportErrorAsync("R5900 Exception", fmt::format("Jump to unaligned address (PC: 0x{:08x})", cpuRegs.pc));
			break;
	}

	VMManager::SetPaused(true);
	recExitExecution();
}

static __ri void ClearRecLUT(BASEBLOCK* base, int memsize)
{
	for (int i = 0; i < memsize / 4; i++)
		base[i].SetFnptr((uptr)JITCompile);
}

static void recReserveRAM()
{
	recLutEntries = (Ps2MemSize::ExposedRam + Ps2MemSize::Rom + Ps2MemSize::Rom1 + Ps2MemSize::Rom2) / 4;

	if (recRAMCopy.size() != Ps2MemSize::ExposedRam)
		recRAMCopy.resize(Ps2MemSize::ExposedRam);

	if (recLutReserve_RAM.size() != recLutEntries)
		recLutReserve_RAM.resize(recLutEntries);

	recLutUnmapped.resize(_64kb / 4);

	BASEBLOCK* basepos = recLutReserve_RAM.data();
	recRAM = basepos;
	basepos += (Ps2MemSize::ExposedRam / 4);
	recROM = basepos;
	basepos += (Ps2MemSize::Rom / 4);
	recROM1 = basepos;
	basepos += (Ps2MemSize::Rom1 / 4);
	recROM2 = basepos;
	basepos += (Ps2MemSize::Rom2 / 4);

	BASEBLOCK* unmapped = recLutUnmapped.data();
	for (int i = 0; i < 0x10000; i++)
	{
		recLUT_SetPage(recLUT, hwLUT, unmapped, i, 0, 0);
	}

	for (int i = 0x0000; i < (int)(Ps2MemSize::ExposedRam / 0x10000); i++)
	{
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0x0000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0x2000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0x3000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0x8000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0xa000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0xb000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0xc000, i, i);
		recLUT_SetPage(recLUT, hwLUT, recRAM, 0xd000, i, i);
	}

	for (int i = 0x1fc0; i < 0x2000; i++)
	{
		recLUT_SetPage(recLUT, hwLUT, recROM, 0x0000, i, i - 0x1fc0);
		recLUT_SetPage(recLUT, hwLUT, recROM, 0x8000, i, i - 0x1fc0);
		recLUT_SetPage(recLUT, hwLUT, recROM, 0xa000, i, i - 0x1fc0);
	}

	for (int i = 0x1e00; i < 0x1e40; i++)
	{
		recLUT_SetPage(recLUT, hwLUT, recROM1, 0x0000, i, i - 0x1e00);
		recLUT_SetPage(recLUT, hwLUT, recROM1, 0x8000, i, i - 0x1e00);
		recLUT_SetPage(recLUT, hwLUT, recROM1, 0xa000, i, i - 0x1e00);
	}

	for (int i = 0x1e40; i < 0x1e80; i++)
	{
		recLUT_SetPage(recLUT, hwLUT, recROM2, 0x0000, i, i - 0x1e40);
		recLUT_SetPage(recLUT, hwLUT, recROM2, 0x8000, i, i - 0x1e40);
		recLUT_SetPage(recLUT, hwLUT, recROM2, 0xa000, i, i - 0x1e40);
	}
}

static void recReserve()
{
	recPtr = SysMemory::GetEERec();
	recPtrEnd = SysMemory::GetEERecEnd() - _64kb;
	recReserveRAM();

	pxAssertRel(!s_pInstCache, "InstCache not allocated");
	s_nInstCacheSize = 128;
	s_pInstCache = (EEINST*)malloc(sizeof(EEINST) * s_nInstCacheSize);
	if (!s_pInstCache)
		pxFailRel("Failed to allocate R5900 InstCache array");
}

alignas(16) static u16 manual_page[Ps2MemSize::TotalRam >> 12];
alignas(16) static u8 manual_counter[Ps2MemSize::TotalRam >> 12];

static void recResetRaw()
{
	Console.WriteLn(Color_StrongBlack, "EE/iR5900 Recompiler Reset");

	if (CHECK_EXTRAMEM != extraRam)
	{
		recReserveRAM();
		extraRam = !extraRam;
	}

	EE::Profiler.Reset();

	xSetTextPtr(R5900_TEXTPTR);
	xSetPtr(SysMemory::GetEERec());
	_DynGen_Dispatchers();
	vtlb_DynGenDispatchers();
	recPtr = xGetPtr();

	ClearRecLUT(recLutReserve_RAM.data(),
		Ps2MemSize::ExposedRam + Ps2MemSize::Rom + Ps2MemSize::Rom1 + Ps2MemSize::Rom2);

	for (int i = 0; i < _64kb / 4; i++)
		recLutUnmapped.data()[i].SetFnptr((uptr)UnmappedRecLUTPage);

	recRAMCopy.fill(0);

	maxrecmem = 0;

	if (s_pInstCache)
		memset(s_pInstCache, 0, sizeof(EEINST) * s_nInstCacheSize);

	recBlocks.Reset();
	vtlb_ClearLoadStoreInfo();

	g_branch = 0;
	g_resetEeScalingStats = true;

	memset(manual_page, 0, sizeof(manual_page));
	memset(manual_counter, 0, sizeof(manual_counter));
}

void recShutdown()
{
	recRAMCopy.deallocate();
	recLutReserve_RAM.deallocate();

	recBlocks.Reset();

	recRAM = recROM = recROM1 = recROM2 = nullptr;

	safe_free(s_pInstCache);
	s_nInstCacheSize = 0;

	recPtr = nullptr;
	recPtrEnd = nullptr;
}

void recStep()
{
}

static fastjmp_buf m_SetJmp_StateCheck;

static void recExitExecution()
{
	fastjmp_jmp(&m_SetJmp_StateCheck, 1);
}

static void recSafeExitExecution()
{
	eeRecExitRequested = true;

	if (!eeEventTestIsActive)
	{
		cpuRegs.nextEventCycle = 0;
	}
	else
	{
		if (psxRegs.iopCycleEE > 0)
		{
			psxRegs.iopBreak += psxRegs.iopCycleEE;
			psxRegs.iopCycleEE = 0;
		}
	}
}

static void recResetEE()
{
	if (eeCpuExecuting)
	{
		eeRecNeedsReset = true;
		recSafeExitExecution();
		return;
	}

	recResetRaw();
}

static void recCancelInstruction()
{
	pxFailRel("recCancelInstruction() called, this should never happen!");
}

static void recExecute()
{
	if (eeRecNeedsReset)
	{
		eeRecNeedsReset = false;
		recResetRaw();
	}

	if (!fastjmp_set(&m_SetJmp_StateCheck))
	{
		eeCpuExecuting = true;
		((void (*)())EnterRecompiledCode)();

	}

	eeCpuExecuting = false;

	EE::Profiler.Print();
}

void R5900::Dynarec::OpcodeImpl::recSYSCALL()
{
	EE::Profiler.EmitOp(eeOpcode::SYSCALL);
	if (GPR_IS_CONST1(3))
	{
		if (g_cpuConstRegs[3].UC[0] == 0x64 || g_cpuConstRegs[3].UC[0] == 0x68)
		{
			s_nBlockCycles += 5650;
			return;
		}
	}
	recCall(R5900::Interpreter::OpcodeImpl::SYSCALL);
	g_branch = 2;
}

void R5900::Dynarec::OpcodeImpl::recBREAK()
{
	EE::Profiler.EmitOp(eeOpcode::BREAK);

	recCall(R5900::Interpreter::OpcodeImpl::BREAK);
	g_branch = 2;
}

void recClear(u32 addr, u32 size)
{
	if ((addr) >= maxrecmem || !(recLUT[(addr) >> 16] + (addr & ~0xFFFFUL)))
		return;
	addr = HWADDR(addr);

	int blockidx = recBlocks.LastIndex(addr + size * 4 - 4);

	if (blockidx == -1)
		return;

	u32 lowerextent = static_cast<u32>(-1), upperextent = 0, ceiling = static_cast<u32>(-1);

	BASEBLOCKEX* pexblock = recBlocks[blockidx + 1];
	if (pexblock)
		ceiling = pexblock->startpc;

	int toRemoveLast = blockidx;

	while ((pexblock = recBlocks[blockidx]))
	{
		u32 blockstart = pexblock->startpc;
		u32 blockend = pexblock->startpc + pexblock->size * 4;
		BASEBLOCK* pblock = PC_GETBLOCK(blockstart);

		if (pblock == s_pCurBlock)
		{
			if (toRemoveLast != blockidx)
			{
				recBlocks.Remove((blockidx + 1), toRemoveLast);
			}
			toRemoveLast = --blockidx;
			continue;
		}

		if (blockend <= addr)
		{
			lowerextent = std::max(lowerextent, blockend);
			break;
		}

		lowerextent = std::min(lowerextent, blockstart);
		upperextent = std::max(upperextent, blockend);
		pblock->SetFnptr((uptr)JITCompile);

		blockidx--;
	}

	if (toRemoveLast != blockidx)
	{
		recBlocks.Remove((blockidx + 1), toRemoveLast);
	}

	upperextent = std::min(upperextent, ceiling);

	for (int i = 0; (pexblock = recBlocks[i]); i++)
	{
		if (s_pCurBlock == PC_GETBLOCK(pexblock->startpc))
			continue;
		u32 blockend = pexblock->startpc + pexblock->size * 4;
		if ((pexblock->startpc >= addr && pexblock->startpc < addr + size * 4) || (pexblock->startpc < addr && blockend > addr)) [[unlikely]]
		{
			Console.Error("[EE] Impossible block clearing failure");
			pxFail("[EE] Impossible block clearing failure");
		}
	}

	if (upperextent > lowerextent)
		ClearRecLUT(PC_GETBLOCK(lowerextent), upperextent - lowerextent);
}


static int* s_pCode;


void SetBranchReg()
{
	g_branch = 1;

	xMOV(ptr32[&cpuRegs.pc], eax);

	xTEST(eax, 3);
	xForwardJNZ32 unaligned;

	iFlushCall(FLUSH_EVERYTHING);
	iBranchTest();

	unaligned.SetTarget();
	xFastCall((const void*)recError, 1);
}

void SetBranchImm(u32 imm)
{
	g_branch = 1;

	pxAssert(imm);

	iFlushCall(FLUSH_EVERYTHING);
	xMOV(ptr32[&cpuRegs.pc], imm);
	iBranchTest(imm);
}

u8* recBeginThunk()
{
	if (recPtr >= recPtrEnd)
		eeRecNeedsReset = true;

	xSetTextPtr(R5900_TEXTPTR);
	xSetPtr(recPtr);
	recPtr = xGetAlignedCallTarget();

	x86Ptr = recPtr;
	return recPtr;
}

u8* recEndThunk()
{
	u8* block_end = x86Ptr;

	pxAssert(block_end < SysMemory::GetEERecEnd());
	recPtr = block_end;
	return block_end;
}

bool TrySwapDelaySlot(u32 rs, u32 rt, u32 rd, bool allow_loadstore)
{
#if 1
	if (g_recompilingDelaySlot)
		return false;

	const u32 opcode_encoded = *(u32*)PSM(pc);
	if (opcode_encoded == 0)
	{
		recompileNextInstruction(true, true);
		return true;
	}

	const u32 opcode_rs = ((opcode_encoded >> 21) & 0x1F);
	const u32 opcode_rt = ((opcode_encoded >> 16) & 0x1F);
	const u32 opcode_rd = ((opcode_encoded >> 11) & 0x1F);

	switch (opcode_encoded >> 26)
	{
		case 8:
		case 9:
		case 10:
		case 11:
		case 12:
		case 13:
		case 14:
		case 24:
		case 25:
		{
			if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && (rd == opcode_rs || rd == opcode_rt)))
				goto is_unsafe;
		}
		break;

		case 26:
		case 27:
		case 30:
		case 31:
		case 32:
		case 33:
		case 34:
		case 35:
		case 36:
		case 37:
		case 38:
		case 39:
		case 40:
		case 41:
		case 42:
		case 43:
		case 44:
		case 45:
		case 46:
		case 55:
		case 63:
		{
			if (!allow_loadstore || (rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && (rd == opcode_rs || rd == opcode_rt)))
				goto is_unsafe;
		}
		break;

		case 15:
		{
			if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && rd == opcode_rt))
				goto is_unsafe;
		}
		break;

		case 49:
		case 57:
		case 54:
		case 62:
			break;

		case 0:
		{
			switch (opcode_encoded & 0x3F)
			{
				case 0:
				case 2:
				case 3:
				case 4:
				case 6:
				case 7:
				case 10:
				case 11:
				case 20:
				case 22:
				case 23:
				case 24:
				case 25:
				case 32:
				case 33:
				case 34:
				case 35:
				case 36:
				case 37:
				case 38:
				case 39:
				case 42:
				case 43:
				case 44:
				case 45:
				case 46:
				case 47:
				case 56:
				case 58:
				case 59:
				case 60:
				case 62:
				case 64:
				{
					if ((rs != 0 && rs == opcode_rd) || (rt != 0 && rt == opcode_rd) || (rd != 0 && (rd == opcode_rs || rd == opcode_rt)))
						goto is_unsafe;
				}
				break;

				case 15:
				case 26:
				case 27:
					break;

				default:
					goto is_unsafe;
			}
		}
		break;

		case 16:
		{
			switch ((opcode_encoded >> 21) & 0x1F)
			{
				case 0:
				case 2:
				{
					if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && rd == opcode_rt))
						goto is_unsafe;
				}
				break;

				case 4:
				case 6:
					break;

				case 16:
				default:
					goto is_unsafe;
			}
			break;
		}
		break;

		case 17:
		{
			switch ((opcode_encoded >> 21) & 0x1F)
			{
				case 0:
				case 2:
				{
					if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && rd == opcode_rt))
						goto is_unsafe;
				}
				break;

				case 4:
				case 6:
				case 16:
				{
					const u32 funct = (opcode_encoded & 0x3F);
					if (funct == 50 || funct == 52 || funct == 54)
					{
						goto is_unsafe;
					}
				}
					[[fallthrough]];

				case 20:
				{
				}
				break;

				default:
					goto is_unsafe;
			}
		}
		break;

		case 18:
		{
			switch ((opcode_encoded >> 21) & 0x1F)
			{
				case 8:
					goto is_unsafe;

				case 1:
				case 2:
				{
					if ((rs != 0 && rs == opcode_rt) || (rt != 0 && rt == opcode_rt) || (rd != 0 && rd == opcode_rt))
						goto is_unsafe;
				}
				break;

				default:
					break;
			}

		}
		break;

		case 28:
		{
			switch (opcode_encoded & 0x3F)
			{
				case 8:
				case 9:
				case 10:
				case 40:
				case 41:
				case 52:
				case 54:
				case 55:
				case 60:
				case 62:
				case 63:
				{
					if ((rs != 0 && rs == opcode_rd) || (rt != 0 && rt == opcode_rd) || (rd != 0 && rd == opcode_rd))
						goto is_unsafe;
				}
				break;

				default:
					goto is_unsafe;
			}
		}
		break;

		default:
			goto is_unsafe;
	}

	recompileNextInstruction(true, true);
	return true;

is_unsafe:
	return false;
#else
	return false;
#endif
}

void SaveBranchState()
{
	s_savenBlockCycles = s_nBlockCycles;
	memcpy(s_saveConstRegs, g_cpuConstRegs, sizeof(g_cpuConstRegs));
	s_saveHasConstReg = g_cpuHasConstReg;
	s_saveFlushedConstReg = g_cpuFlushedConstReg;
	s_psaveInstInfo = g_pCurInstInfo;

	memcpy(s_saveXMMregs, xmmregs, sizeof(xmmregs));
}

void LoadBranchState()
{
	s_nBlockCycles = s_savenBlockCycles;

	memcpy(g_cpuConstRegs, s_saveConstRegs, sizeof(g_cpuConstRegs));
	g_cpuHasConstReg = s_saveHasConstReg;
	g_cpuFlushedConstReg = s_saveFlushedConstReg;
	g_pCurInstInfo = s_psaveInstInfo;

	memcpy(xmmregs, s_saveXMMregs, sizeof(xmmregs));
}

void iFlushCall(int flushtype)
{
	for (u32 i = 0; i < iREGCNT_GPR; i++)
	{
		if (!x86regs[i].inuse)
			continue;

		if (xRegisterBase::IsCallerSaved(i) ||
			((flushtype & FLUSH_FREE_VU0) && x86regs[i].type == X86TYPE_VIREG) ||
			((flushtype & FLUSH_FREE_NONTEMP_X86) && x86regs[i].type != X86TYPE_TEMP) ||
			((flushtype & FLUSH_FREE_TEMP_X86) && x86regs[i].type == X86TYPE_TEMP))
		{
			_freeX86reg(i);
		}
	}

	for (u32 i = 0; i < iREGCNT_XMM; i++)
	{
		if (!xmmregs[i].inuse)
			continue;

		if (xRegisterSSE::IsCallerSaved(i) ||
			(flushtype & FLUSH_FREE_XMM) ||
			((flushtype & FLUSH_FREE_VU0) && xmmregs[i].type == XMMTYPE_VFREG))
		{
			_freeXMMreg(i);
		}
	}

	if (flushtype & FLUSH_ALL_X86)
		_flushX86regs();

	if (flushtype & FLUSH_FLUSH_XMM)
		_flushXMMregs();

	if (flushtype & FLUSH_CONSTANT_REGS)
		_flushConstRegs(true);

	if ((flushtype & FLUSH_PC) && !g_cpuFlushedPC)
	{
		xMOV(ptr32[&cpuRegs.pc], pc);
		g_cpuFlushedPC = true;
	}

	if ((flushtype & FLUSH_CODE) && !g_cpuFlushedCode)
	{
		xMOV(ptr32[&cpuRegs.code], cpuRegs.code);
		g_cpuFlushedCode = true;
	}

#if 0
	if ((flushtype == FLUSH_CAUSE) && !g_maySignalException)
	{
		if (g_recompilingDelaySlot)
			xOR(ptr32[&cpuRegs.CP0.n.Cause], 1 << 31);
		g_maySignalException = true;
	}
#endif
}

#define DEFAULT_SCALED_BLOCKS() (s_nBlockCycles >> 3)

static u32 scaleblockcycles_calculation()
{
	const bool lowcycles = (s_nBlockCycles <= 40);
	const s8 cyclerate = EmuConfig.Speedhacks.EECycleRate;
	u32 scale_cycles = 0;

	if (cyclerate == 0 || lowcycles || cyclerate < -99 || cyclerate > 3)
		scale_cycles = DEFAULT_SCALED_BLOCKS();

	else if (cyclerate > 1)
		scale_cycles = s_nBlockCycles >> (2 + cyclerate);

	else if (cyclerate == 1)
		scale_cycles = DEFAULT_SCALED_BLOCKS() / 1.3f;

	else if (cyclerate == -1)
		scale_cycles = (s_nBlockCycles <= 80 || s_nBlockCycles > 168 ? 5 : 7) * s_nBlockCycles / 32;

	else
		scale_cycles = ((5 + (-2 * (cyclerate + 1))) * s_nBlockCycles) >> 5;

	return (scale_cycles < 1) ? 1 : scale_cycles;
}

static u32 scaleblockcycles()
{
	const u32 scaled = scaleblockcycles_calculation();

#if 0
	static u32 scaled_overall = 0, unscaled_overall = 0;
	if (g_resetEeScalingStats)
	{
		scaled_overall = unscaled_overall = 0;
		g_resetEeScalingStats = false;
	}
	u32 unscaled = DEFAULT_SCALED_BLOCKS();
	if (!unscaled) unscaled = 1;

	scaled_overall += scaled;
	unscaled_overall += unscaled;
	float ratio = static_cast<float>(unscaled_overall) / scaled_overall;

	DevCon.WriteLn(L"Unscaled overall: %d,  scaled overall: %d,  relative EE clock speed: %d %%",
	               unscaled_overall, scaled_overall, static_cast<int>(100 * ratio));
#endif

	return scaled;
}
u32 scaleblockcycles_clear()
{
	u32 scaled = scaleblockcycles_calculation();

#if 0
	static u32 scaled_overall = 0, unscaled_overall = 0;
	if (g_resetEeScalingStats)
	{
		scaled_overall = unscaled_overall = 0;
		g_resetEeScalingStats = false;
	}
	u32 unscaled = DEFAULT_SCALED_BLOCKS();
	if (!unscaled) unscaled = 1;

	scaled_overall += scaled;
	unscaled_overall += unscaled;
	float ratio = static_cast<float>(unscaled_overall) / scaled_overall;

	DevCon.WriteLn(L"Unscaled overall: %d,  scaled overall: %d,  relative EE clock speed: %d %%",
		unscaled_overall, scaled_overall, static_cast<int>(100 * ratio));
#endif
	const s8 cyclerate = EmuConfig.Speedhacks.EECycleRate;
	const bool lowcycles = (s_nBlockCycles <= 40);

	if (!lowcycles && cyclerate > 1)
	{
		s_nBlockCycles &= (0x1 << (cyclerate + 2)) - 1;
	}
	else
	{
		s_nBlockCycles &= 0x7;
	}

	return scaled;
}

static void iBranchTest(u32 newpc)
{

	if (EmuConfig.Speedhacks.WaitLoop && s_nBlockFF && newpc == s_branchTo)
	{
		xMOV(rax, ptr64[&cpuRegs.nextEventCycle]);
		xADD(ptr64[&cpuRegs.cycle], scaleblockcycles());
		xCMP(rax, ptr64[&cpuRegs.cycle]);
		xCMOVS(rax, ptr64[&cpuRegs.cycle]);
		xMOV(ptr64[&cpuRegs.cycle], rax);

		xJMP((void*)DispatcherEvent);
	}
	else
	{
		xMOV(rax, ptr64[&cpuRegs.cycle]);
		xADD(rax, scaleblockcycles());
		xMOV(ptr64[&cpuRegs.cycle], rax);
		xSUB(rax, ptr64[&cpuRegs.nextEventCycle]);

		if (newpc == 0xffffffff)
			xJS(DispatcherReg);
		else
			recBlocks.Link(HWADDR(newpc), xJcc32(Jcc_Signed));

		xJMP((void*)DispatcherEvent);
	}
}

int cop2flags(u32 code)
{
	if (code >> 26 != 022)
		return 0;
	if ((code >> 25 & 1) == 0)
		return 0;

	switch (code >> 2 & 15)
	{
		case 15:
			switch (code >> 6 & 0x1f)
			{
				case 4:
				case 5:
				case 12:
				case 13:
				case 15:
				case 16:
					return 0;
				case 7:
					if ((code & 3) == 1)
						return 0;
					if ((code & 3) == 3)
						return 4;
					return 3;
				case 11:
					if ((code & 3) == 3)
						return 0;
					return 3;
				case 14:
					if ((code & 3) == 3)
						return 0;
					return 1;
				default:
					break;
			}
			break;
		case 4:
		case 5:
		case 12:
		case 13:
		case 14:
			return 0;
		case 7:
			if ((code & 1) == 1)
				return 0;
			return 3;
		case 10:
			if ((code & 3) == 3)
				return 0;
			return 3;
		case 11:
			if ((code & 3) == 3)
				return 0;
			return 3;
		default:
			break;
	}
	return 3;
}

int COP2DivUnitTimings(u32 code)
{
	switch (code & 0x3FF)
	{
		case 0x3BC:
		case 0x3BD:
			return 6;
		case 0x3BE:
			return 12;
		default:
			return 0;
	}
}

bool COP2IsQOP(u32 code)
{
	if (_Opcode_ != 022)
		return false;

	if ((code & 0x3f) == 0x20)
		return true;
	if ((code & 0x3f) == 0x21)
		return true;
	if ((code & 0x3f) == 0x24)
		return true;
	if ((code & 0x3f) == 0x25)
		return true;
	if ((code & 0x3f) == 0x1C)
		return true;
	if ((code & 0x7FF) == 0x1FC)
		return true;
	if ((code & 0x7FF) == 0x23C)
		return true;
	if ((code & 0x7FF) == 0x23D)
		return true;
	if ((code & 0x7FF) == 0x27C)
		return true;
	if ((code & 0x7FF) == 0x27D)
		return true;

	return false;
}


void dynarecCheckBreakpoint()
{
	u32 pc = cpuRegs.pc;
	if (CBreakPoints::CheckSkipFirst(BREAKPOINT_EE, pc) != 0)
	{
		CBreakPoints::ClearSkipFirst(BREAKPOINT_EE);
		return;
	}

	const int bpFlags = isBreakpointNeeded(pc);
	bool hit = false;
	if (bpFlags & 1)
	{
		auto cond = CBreakPoints::GetBreakPointCondition(BREAKPOINT_EE, pc);
		if (cond == NULL || cond->Evaluate())
		{
			hit = true;
		}
	}
	if (bpFlags & 2)
	{
		auto cond = CBreakPoints::GetBreakPointCondition(BREAKPOINT_EE, pc + 4);
		if (cond == NULL || cond->Evaluate())
			hit = true;
	}

	if (!hit)
		return;

	CBreakPoints::SetBreakpointTriggered(true, BREAKPOINT_EE);
	VMManager::SetPaused(true);
	recExitExecution();
}

void dynarecMemcheck(size_t i)
{
	const u32 op = memRead32(cpuRegs.pc);
	const OPCODE& opcode = GetInstruction(op);
	if (CBreakPoints::CheckSkipFirst(BREAKPOINT_EE, pc) != 0)
	{
		CBreakPoints::ClearSkipFirst(BREAKPOINT_EE);
		return;
	}

	auto mc = CBreakPoints::GetMemChecks(BREAKPOINT_EE)[i];

	if (mc.hasCond)
	{
		if (!mc.cond.Evaluate())
			return;
	}

	if (mc.result & MEMCHECK_LOG)
	{
		if (opcode.flags & IS_STORE)
			DevCon.WriteLn("Hit store breakpoint @0x%x", cpuRegs.pc);
		else
			DevCon.WriteLn("Hit load breakpoint @0x%x", cpuRegs.pc);
	}

	CBreakPoints::SetBreakpointTriggered(true, BREAKPOINT_EE);
	VMManager::SetPaused(true);
	recExitExecution();
}

void recMemcheck(u32 op, u32 bits, bool store)
{
	iFlushCall(FLUSH_EVERYTHING | FLUSH_PC);

	_eeMoveGPRtoR(ecx, (op >> 21) & 0x1F, false);
	if (static_cast<s16>(op) != 0)
		xADD(ecx, static_cast<s16>(op));
	if (bits == 128)
		xAND(ecx, ~0x0F);

	xFastCall((void*)standardizeBreakpointAddress, ecx);
	xMOV(ecx, eax);
	xMOV(edx, eax);
	xADD(edx, bits / 8);

	auto checks = CBreakPoints::GetMemChecks(BREAKPOINT_EE);
	for (size_t i = 0; i < checks.size(); i++)
	{
		if (checks[i].result == 0)
			continue;
		if ((checks[i].memCond & MEMCHECK_WRITE) == 0 && store)
			continue;
		if ((checks[i].memCond & MEMCHECK_READ) == 0 && !store)
			continue;

		xMOV(eax, standardizeBreakpointAddress(checks[i].end));
		xCMP(ecx, eax);
		xForwardJGE8 next1;

		xMOV(eax, standardizeBreakpointAddress(checks[i].start));
		xCMP(eax, edx);
		xForwardJGE8 next2;

		if (checks[i].result & MEMCHECK_BREAK)
		{
			xMOV(eax, i);
			xFastCall((void*)dynarecMemcheck, eax);
		}

		next1.SetTarget();
		next2.SetTarget();
	}
}

bool encodeBreakpoint()
{
	if (isBreakpointNeeded(pc) != 0)
	{
		iFlushCall(FLUSH_EVERYTHING | FLUSH_PC);
		xFastCall((void*)dynarecCheckBreakpoint);
		return true;
	}
	return false;
}

bool encodeMemcheck()
{
	const int needed = isMemcheckNeeded(pc);
	if (needed == 0)
		return false;

	const u32 op = memRead32(needed == 2 ? pc + 4 : pc);
	const OPCODE& opcode = GetInstruction(op);

	const bool store = (opcode.flags & IS_STORE) != 0;
	switch (opcode.flags & MEMTYPE_MASK)
	{
		case MEMTYPE_BYTE:
			recMemcheck(op, 8, store);
			break;
		case MEMTYPE_HALF:
			recMemcheck(op, 16, store);
			break;
		case MEMTYPE_WORD:
			recMemcheck(op, 32, store);
			break;
		case MEMTYPE_DWORD:
			recMemcheck(op, 64, store);
			break;
		case MEMTYPE_QWORD:
			recMemcheck(op, 128, store);
			break;
	}
	return true;
}

void recompileNextInstruction(bool delayslot, bool swapped_delay_slot)
{
	if (EmuConfig.EnablePatches)
		Patch::ApplyDynamicPatches(pc);

	if (!delayslot)
	{
		if(encodeBreakpoint() || encodeMemcheck())
			xFastCall((void*)CBreakPoints::CommitClearSkipFirst, BREAKPOINT_EE);
	}
	else
	{
#ifdef DUMP_BLOCKS
		std::string disasm;
		disR5900Fasm(disasm, *(u32*)PSM(pc), pc, false);
		fprintf(stderr, "Compiling delay slot %08X %s\n", pc, disasm.c_str());
#endif

		_clearNeededX86regs();
		_clearNeededXMMregs();
	}

	s_pCode = (int*)PSM(pc);
	pxAssert(s_pCode);

#if 0
	if (IsDevBuild)
		xNOP();
	if (IsDebugBuild)
		xMOV(eax, pc);
#endif

	const int old_code = cpuRegs.code;
	EEINST* old_inst_info = g_pCurInstInfo;

	cpuRegs.code = *(int*)s_pCode;

	if (!delayslot)
	{
		pc += 4;
		g_cpuFlushedPC = false;
		g_cpuFlushedCode = false;
	}
	else
	{
		g_recompilingDelaySlot = true;
	}

	g_pCurInstInfo++;

	if (pc <= s_nEndBlock && (g_pCurInstInfo + (s_nEndBlock - pc) / 4 + 1) <= s_pInstCache + s_nInstCacheSize)
	{
		int count;
		for (u32 i = 0; i < iREGCNT_GPR; ++i)
		{
			if (x86regs[i].inuse)
			{
				count = _recIsRegReadOrWritten(g_pCurInstInfo, (s_nEndBlock - pc) / 4 + 1, x86regs[i].type, x86regs[i].reg);
				if (count > 0)
					x86regs[i].counter = 1000 - count;
				else
					x86regs[i].counter = 0;
			}
		}

		for (u32 i = 0; i < iREGCNT_XMM; ++i)
		{
			if (xmmregs[i].inuse)
			{
				count = _recIsRegReadOrWritten(g_pCurInstInfo, (s_nEndBlock - pc) / 4 + 1, xmmregs[i].type, xmmregs[i].reg);
				if (count > 0)
					xmmregs[i].counter = 1000 - count;
				else
					xmmregs[i].counter = 0;
			}
		}
	}

	if (g_pCurInstInfo->info & EEINST_COP2_FLUSH_VU0_REGISTERS)
	{
		RALOG("Flushing cop2 registers\n");
		_flushCOP2regs();
	}

	const OPCODE& opcode = GetCurrentInstruction();

	if (delayslot)
	{
		bool check_branch_delay = false;
		switch (_Opcode_)
		{
			case 0:
				switch (_Funct_)
				{
					case 8:
					case 9:
						check_branch_delay = true;
						break;
				}
				break;

			case 1:
				switch (_Rt_)
				{
					case 0:
					case 1:
					case 2:
					case 3:
					case 0x10:
					case 0x11:
					case 0x12:
					case 0x13:
						check_branch_delay = true;
						break;
				}
				break;

			case 2:
			case 3:
			case 4:
			case 5:
			case 6:
			case 7:
			case 0x14:
			case 0x15:
			case 0x16:
			case 0x17:
				check_branch_delay = true;
				break;
		}
		if (check_branch_delay)
		{
			DevCon.Warning("Branch %x in delay slot!", cpuRegs.code);
			_clearNeededX86regs();
			_clearNeededXMMregs();
			pc += 4;
			g_cpuFlushedPC = false;
			g_cpuFlushedCode = false;
			if (g_maySignalException)
				xAND(ptr32[&cpuRegs.CP0.n.Cause], ~(1 << 31));

			g_recompilingDelaySlot = false;
			return;
		}
	}
	if (cpuRegs.code == 0x00000000)
	{
		s_nBlockCycles += 9 * (2 - ((cpuRegs.CP0.n.Config >> 18) & 0x1));
	}
	else
	{
		s_nBlockCycles += opcode.cycles * (2 - ((cpuRegs.CP0.n.Config >> 18) & 0x1));
		opcode.recompile();
	}

	if (!swapped_delay_slot)
	{
		_clearNeededX86regs();
		_clearNeededXMMregs();
	}
	_validateRegs();

	if (delayslot)
	{
		pc += 4;
		g_cpuFlushedPC = false;
		g_cpuFlushedCode = false;
		if (g_maySignalException)
			xAND(ptr32[&cpuRegs.CP0.n.Cause], ~(1 << 31));
		g_recompilingDelaySlot = false;
	}

	g_maySignalException = false;

	if (_Opcode_ == 022)
	{
		if ((cpuRegs.code >> 25 & 1) == 1 && (cpuRegs.code >> 2 & 0x1ff) == 0xdf)
			;
		else if (_Rs_ == 6)
			;
		else if ((cpuRegs.code & 0x7FC) == 0x3BC)
		{
			int cycles = COP2DivUnitTimings(cpuRegs.code);
			for (u32 p = pc; cycles > 0 && p < s_nEndBlock; p += 4, cycles--)
			{
				cpuRegs.code = memRead32(p);

				if ((_Opcode_ == 022) && (cpuRegs.code & 0x7FC) == 0x3BC)
					break;

				else if (COP2IsQOP(cpuRegs.code))
				{
					Console.Warning("Possible incorrect Q value used in COP2. If the game is broken, please report to https://github.com/pcsx2/pcsx2.");
					for (u32 i = s_pCurBlockEx->startpc; i < s_nEndBlock; i += 4)
					{
						std::string disasm = "";
						disR5900Fasm(disasm, memRead32(i), i, false);
						Console.Warning("%x %s%08X %s", i, i == pc - 4 ? "*" : i == p ? "=" :
																						" ",
							memRead32(i), disasm.c_str());
					}
					break;
				}
			}
		}
		else
		{
			int s = cop2flags(cpuRegs.code);
			int all_count = 0, cop2o_count = 0, cop2m_count = 0;
			for (u32 p = pc; s != 0 && p < s_nEndBlock && all_count < 10 && cop2m_count < 5 && cop2o_count < 4; p += 4)
			{
				cpuRegs.code = memRead32(p);
				if (_Opcode_ == 022 && _Rs_ == 2)
					if ((_Rd_ == 16 && s & 1) || (_Rd_ == 17 && s & 2) || (_Rd_ == 18 && s & 4))
					{
						std::string disasm;
						Console.Warning("Possible old value used in COP2 code. If the game is broken, please report to https://github.com/pcsx2/pcsx2.");
						for (u32 i = s_pCurBlockEx->startpc; i < s_nEndBlock; i += 4)
						{
							disasm = "";
							disR5900Fasm(disasm, memRead32(i), i, false);
							Console.Warning("%x %s%08X %s", i, i == pc - 4 ? "*" : i == p ? "=" :
																							" ",
								memRead32(i), disasm.c_str());
						}
						break;
					}
				s &= ~cop2flags(cpuRegs.code);
				all_count++;
				if (_Opcode_ == 022 && _Rs_ == 8)
					;
				else if (_Opcode_ == 022 && (cpuRegs.code >> 25 & 1) == 0)
					cop2m_count++;
				else if (_Opcode_ == 022)
					cop2o_count++;
			}
		}
	}
	cpuRegs.code = *s_pCode;

	if (swapped_delay_slot)
	{
		cpuRegs.code = old_code;
		g_pCurInstInfo = old_inst_info;
	}
}

#ifdef TRACE_BLOCKS
static void PreBlockCheck(u32 blockpc)
{
#if 0
	static FILE* fp = nullptr;
	static bool fp_opened = false;
	if (!fp_opened && cpuRegs.cycle >= 0)
	{
		fp = std::fopen("C:\\Dumps\\comp\\reglog.txt", "wb");
		fp_opened = true;
	}
	if (fp)
	{
		u32 hash = crc32(0, (Bytef*)&cpuRegs, offsetof(cpuRegisters, pc));
		u32 hashf = crc32(0, (Bytef*)&fpuRegs, sizeof(fpuRegisters));
		u32 hashi = crc32(0, (Bytef*)&VU0, offsetof(VURegs, idx));

#if 1
		std::fprintf(fp, "%08X (%u; %08X; %08X; %08X):", cpuRegs.pc, cpuRegs.cycle, hash, hashf, hashi);
		for (int i = 0; i < 34; i++)
		{
			std::fprintf(fp, " %s: %08X%08X%08X%08X", R3000A::disRNameGPR[i], cpuRegs.GPR.r[i].UL[3], cpuRegs.GPR.r[i].UL[2], cpuRegs.GPR.r[i].UL[1], cpuRegs.GPR.r[i].UL[0]);
		}
#if 1
		std::fprintf(fp, "\nFPR: CR: %08X ACC: %08X", fpuRegs.fprc[31], fpuRegs.ACC.UL);
		for (int i = 0; i < 32; i++)
			std::fprintf(fp, " %08X", fpuRegs.fpr[i].UL);
#endif
#if 1
		std::fprintf(fp, "\nVF: ");
		for (int i = 0; i < 32; i++)
			std::fprintf(fp, " %u: %08X %08X %08X %08X", i, VU0.VF[i].UL[0], VU0.VF[i].UL[1], VU0.VF[i].UL[2], VU0.VF[i].UL[3]);
		std::fprintf(fp, "\nVI: ");
		for (int i = 0; i < 32; i++)
			std::fprintf(fp, " %u: %08X", i, VU0.VI[i].UL);
		std::fprintf(fp, "\nACC: %08X %08X %08X %08X Q: %08X P: %08X", VU0.ACC.UL[0], VU0.ACC.UL[1], VU0.ACC.UL[2], VU0.ACC.UL[3], VU0.q.UL, VU0.p.UL);
		std::fprintf(fp, " MAC %08X %08X %08X %08X", VU0.micro_macflags[3], VU0.micro_macflags[2], VU0.micro_macflags[1], VU0.micro_macflags[0]);
		std::fprintf(fp, " CLIP %08X %08X %08X %08X", VU0.micro_clipflags[3], VU0.micro_clipflags[2], VU0.micro_clipflags[1], VU0.micro_clipflags[0]);
		std::fprintf(fp, " STATUS %08X %08X %08X %08X", VU0.micro_statusflags[3], VU0.micro_statusflags[2], VU0.micro_statusflags[1], VU0.micro_statusflags[0]);
#endif
		std::fprintf(fp, "\n");
#else
		std::fprintf(fp, "%08X (%u): %08X %08X %08X\n", cpuRegs.pc, cpuRegs.cycle, hash, hashf, hashi);
#endif
	}
#endif
#if 0
	if (cpuRegs.cycle == 0)
		pauseAAA();
#endif
}
#endif

void dyna_block_discard(u32 start, u32 sz)
{
	eeRecPerfLog.Write(Color_StrongGray, "Clearing Manual Block @ 0x%08X  [size=%d]", start, sz * 4);
	recClear(start, sz);
}

void dyna_page_reset(u32 start, u32 sz)
{
	recClear(start & ~0xfffUL, 0x400);
	manual_counter[start >> 12]++;
	mmap_MarkCountedRamPage(start);
}

static void memory_protect_recompiled_code(u32 startpc, u32 size)
{
	u32 inpage_ptr = HWADDR(startpc);
	const u32 inpage_sz = size * 4;

	const bool contains_thread_stack = ((startpc >> 12) == 0x81) || ((startpc >> 12) == 0x80001);

	const vtlb_ProtectionMode PageType = contains_thread_stack ? ProtMode_Manual : mmap_GetRamPageInfo(inpage_ptr);

	switch (PageType)
	{
		case ProtMode_NotRequired:
			break;

		case ProtMode_None:
		case ProtMode_Write:
			mmap_MarkCountedRamPage(inpage_ptr);
			manual_page[inpage_ptr >> 12] = 0;
			break;

		case ProtMode_Manual:
			xMOV(arg1regd, inpage_ptr);
			xMOV(arg2regd, inpage_sz / 4);

			u32 lpc = inpage_ptr;
			u32 stg = inpage_sz;

			while (stg > 0)
			{
				xCMP(ptr32[PSM(lpc)], *(u32*)PSM(lpc));
				xJNE(DispatchBlockDiscard);

				stg -= 4;
				lpc += 4;
			}

			if (!contains_thread_stack && manual_counter[inpage_ptr >> 12] <= 3)
			{

				xADD(ptr16[&manual_page[inpage_ptr >> 12]], size);
				xJC(DispatchPageReset);

				eeRecPerfLog.Write("Manual block @ %08X : size =%3d  page/offs = 0x%05X/0x%03X  inpgsz = %d  clearcnt = %d",
					startpc, size, inpage_ptr >> 12, inpage_ptr & 0xfff, inpage_sz, manual_counter[inpage_ptr >> 12]);
			}
			else
			{
				eeRecPerfLog.Write("Uncounted Manual block @ 0x%08X : size =%3d page/offs = 0x%05X/0x%03X  inpgsz = %d",
					startpc, size, inpage_ptr >> 12, inpage_ptr & 0xfff, inpage_sz);
			}
			break;
	}
}

static bool skipMPEG_By_Pattern(u32 sPC)
{

	if (!CHECK_SKIPMPEGHACK)
		return 0;

	if ((s_nEndBlock == sPC + 12) && (memRead32(sPC + 4) == 0x03e00008))
	{
		const u32 code = memRead32(sPC);
		const u32 p1 = 0x8c800040;
		const u32 p2 = 0x8c020000 | (code & 0x1f0000) << 5;
		if ((code & 0xffe0ffff) != p1)
			return 0;
		if (memRead32(sPC + 8) != p2)
			return 0;
		xMOV(ptr32[&cpuRegs.GPR.n.v0.UL[0]], 1);
		xMOV(ptr32[&cpuRegs.GPR.n.v0.UL[1]], 0);
		xMOV(eax, ptr32[&cpuRegs.GPR.n.ra.UL[0]]);
		xMOV(ptr32[&cpuRegs.pc], eax);
		iBranchTest();
		g_branch = 1;
		pc = s_nEndBlock;
		Console.WriteLn(Color_StrongGreen, "sceMpegIsEnd pattern found! Recompiling skip video fix...");
		return 1;
	}
	return 0;
}

static bool recSkipTimeoutLoop(s32 reg, bool is_timeout_loop)
{
	if (!EmuConfig.Speedhacks.WaitLoop || !is_timeout_loop)
		return false;

	DevCon.WriteLn("[EE] Skipping timeout loop at 0x%08X -> 0x%08X", s_pCurBlockEx->startpc, s_nEndBlock);

	xMOV(r12, ptr64[&cpuRegs.cycle]);
	xMOV(rcx, ptr64[&cpuRegs.nextEventCycle]);
	xCMP(r12, rcx);
	xJAE((void*)DispatcherEvent);

	xMOV(edx, ptr32[&cpuRegs.GPR.r[reg].UL[0]]);
	xLEA(rax, ptrNative[rdx * 8 + r12]);
	xCMP(rcx, rax);
	xCMOVB(rax, rcx);
	xMOV(ptr64[&cpuRegs.cycle], rax);
	xSUB(rax, r12);
	xSHR(rax, 3);
	xSUB(rdx, rax);
	xMOV(ptr32[&cpuRegs.GPR.r[reg].UL[0]], edx);
	xJNZ((void*)DispatcherEvent);
	xMOV(ptr32[&cpuRegs.pc], s_nEndBlock);
	recBlocks.Link(HWADDR(s_nEndBlock), xJcc32());

	g_branch = 1;
	pc = s_nEndBlock;

	return true;
}

static void recRecompile(const u32 startpc)
{
	u32 i = 0;
	u32 willbranch3 = 0;

	pxAssert(startpc);

	if (recPtr >= recPtrEnd)
		eeRecNeedsReset = true;

	if (HWADDR(startpc) == VMManager::Internal::GetCurrentELFEntryPoint())
		VMManager::Internal::EntryPointCompilingOnCPUThread();

	if (eeRecNeedsReset)
	{
		eeRecNeedsReset = false;
		recResetRaw();
	}

	xSetTextPtr(R5900_TEXTPTR);
	xSetPtr(recPtr);
	recPtr = xGetAlignedCallTarget();

	s_pCurBlock = PC_GETBLOCK(startpc);

	pxAssert(s_pCurBlock->GetFnptr() == (uptr)JITCompile);

	s_pCurBlockEx = recBlocks.Get(HWADDR(startpc));
	pxAssert(!s_pCurBlockEx || s_pCurBlockEx->startpc != HWADDR(startpc));

	s_pCurBlockEx = recBlocks.New(HWADDR(startpc), (uptr)recPtr);

	pxAssert(s_pCurBlockEx);

	if (HWADDR(startpc) == EELOAD_START)
	{
		const u32 mainjump = memRead32(EELOAD_START + 0x9c);
		if (mainjump >> 26 == 3)
			g_eeloadMain = ((EELOAD_START + 0xa0) & 0xf0000000U) | (mainjump << 2 & 0x0fffffffU);
	}

	if (g_eeloadMain && HWADDR(startpc) == HWADDR(g_eeloadMain))
	{
		xFastCall((void*)eeloadHook);
		if (VMManager::Internal::IsFastBootInProgress())
		{
			const u32 typeAexecjump = memRead32(EELOAD_START + 0x470);
			const u32 typeBexecjump = memRead32(EELOAD_START + 0x5B0);
			const u32 typeCexecjump = memRead32(EELOAD_START + 0x618);
			const u32 typeDexecjump = memRead32(EELOAD_START + 0x600);
			if ((typeBexecjump >> 26 == 3) || (typeCexecjump >> 26 == 3) || (typeDexecjump >> 26 == 3))
				g_eeloadExec = EELOAD_START + 0x2B8;
			else if (typeAexecjump >> 26 == 3)
				g_eeloadExec = EELOAD_START + 0x170;
			else
				Console.WriteLn("recRecompile: Could not enable launch arguments for fast boot mode; unidentified BIOS version! Please report this to the PCSX2 developers.");
		}
	}

	if (g_eeloadExec && HWADDR(startpc) == HWADDR(g_eeloadExec))
		xFastCall((void*)eeloadHook2);

	g_branch = 0;

	s_nBlockCycles = 0;
	s_nBlockInterlocked = false;
	pc = startpc;
	g_cpuHasConstReg = g_cpuFlushedConstReg = 1;
	pxAssert(g_cpuConstRegs[0].UD[0] == 0);

	_initX86regs();
	_initXMMregs();

#ifdef TRACE_BLOCKS
	xFastCall((void*)PreBlockCheck, pc);
#endif

	if (EmuConfig.Gamefixes.GoemonTlbHack)
	{
		if (pc == 0x33ad48 || pc == 0x35060c)
		{
			xFastCall((void*)GoemonPreloadTlb);
		}
		else if (pc == 0x3563b8)
		{
			eeRecNeedsReset = true;
			xFastCall((void*)GoemonUnloadTlb, ptr32[&cpuRegs.GPR.n.a0.UL[0]]);
		}
	}

	i = startpc;
	s_nEndBlock = 0xffffffff;
	s_branchTo = -1;

	s32 timeout_reg = -1;
	bool is_timeout_loop = true;

	const int n1 = isBreakpointNeeded(i);
	const int n2 = isMemcheckNeeded(i);
	const int n = std::max<int>(n1, n2);
	if (n != 0)
	{
		s_nEndBlock = i + n * 4;
		goto StartRecomp;
	}

	while (1)
	{
		BASEBLOCK* pblock = PC_GETBLOCK(i);

		if (isBreakpointNeeded(i) != 0 || isMemcheckNeeded(i) != 0)
		{
			s_nEndBlock = i;
			break;
		}

		if (i != startpc)
		{
			if ((i & 0xffc) == 0x0)
			{
				willbranch3 = 1;
				s_nEndBlock = i;

				eeRecPerfLog.Write("Pagesplit @ %08X : size=%d insts", startpc, (i - startpc) / 4);
				break;
			}

			if (pblock->GetFnptr() != (uptr)JITCompile)
			{
				willbranch3 = 1;
				s_nEndBlock = i;
				break;
			}
		}

		cpuRegs.code = *(int*)PSM(i);

		if (is_timeout_loop)
		{
			if ((cpuRegs.code >> 26) == 8 || (cpuRegs.code >> 26) == 9)
			{
				if (timeout_reg >= 0 || _Rs_ != _Rt_ || _Imm_ >= 0)
					is_timeout_loop = false;
				else
					timeout_reg = _Rs_;
			}
			else if ((cpuRegs.code >> 26) == 5)
			{
				if (timeout_reg != static_cast<s32>(_Rs_) || _Rt_ != 0 || memRead32(i + 4) != 0)
					is_timeout_loop = false;
			}
			else if (cpuRegs.code != 0)
			{
				is_timeout_loop = false;
			}
		}

		switch (cpuRegs.code >> 26)
		{
			case 0:
				if (_Funct_ == 8 || _Funct_ == 9)
				{
					s_nEndBlock = i + 8;
					goto StartRecomp;
				}
				else if (_Funct_ == 12 || _Funct_ == 13)
				{
					s_nEndBlock = i + 4;
					goto StartRecomp;
				}
				break;

			case 1:

				if (_Rt_ < 4 || (_Rt_ >= 16 && _Rt_ < 20))
				{
					s_branchTo = _Imm_ * 4 + i + 4;
					if (s_branchTo > startpc && s_branchTo < i)
						s_nEndBlock = s_branchTo;
					else
						s_nEndBlock = i + 8;

					goto StartRecomp;
				}
				break;

			case 2:
			case 3:
				s_branchTo = (_InstrucTarget_ << 2) | ((i + 4) & 0xf0000000);
				s_nEndBlock = i + 8;
				goto StartRecomp;

			case 4:
			case 5:
			case 6:
			case 7:
			case 20:
			case 21:
			case 22:
			case 23:
				s_branchTo = _Imm_ * 4 + i + 4;
				if (s_branchTo > startpc && s_branchTo < i)
					s_nEndBlock = s_branchTo;
				else
					s_nEndBlock = i + 8;

				goto StartRecomp;

			case 16:
				if (_Rs_ == 16)
				{
					if (_Funct_ == 24)
					{
						s_nEndBlock = i + 4;
						goto StartRecomp;
					}
				}
				// Fall through!

			case 17:
			case 18:
				if (_Rs_ == 8)
				{
					s_branchTo = _Imm_ * 4 + i + 4;
					if (s_branchTo > startpc && s_branchTo < i)
						s_nEndBlock = s_branchTo;
					else
						s_nEndBlock = i + 8;

					goto StartRecomp;
				}
				break;
		}

		i += 4;
	}

StartRecomp:

	s_nBlockFF = false;
	if (s_branchTo == startpc)
	{
		s_nBlockFF = true;

		u32 reads = 0, loads = 1;

		for (i = startpc; i < s_nEndBlock; i += 4)
		{
			if (i == s_nEndBlock - 8)
				continue;
			cpuRegs.code = *(u32*)PSM(i);
			if (cpuRegs.code == 0)
				continue;
			else if (_Opcode_ == 057 || (_Opcode_ == 0 && _Funct_ == 017))
				continue;
			else if ((_Opcode_ & 070) == 010 || (_Opcode_ & 076) == 030)
			{
				if (loads & 1 << _Rs_)
				{
					loads |= 1 << _Rt_;
					continue;
				}
				else
					reads |= 1 << _Rs_;
				if (reads & 1 << _Rt_)
				{
					s_nBlockFF = false;
					break;
				}
			}
			else if (_Opcode_ == 0 && (_Funct_ & 060) == 040 && (_Funct_ & 076) != 050)
			{
				if (loads & 1 << _Rs_ && loads & 1 << _Rt_)
				{
					loads |= 1 << _Rd_;
					continue;
				}
				else
					reads |= 1 << _Rs_ | 1 << _Rt_;
				if (reads & 1 << _Rd_)
				{
					s_nBlockFF = false;
					break;
				}
			}
			else if ((_Opcode_ & 070) == 040 || (_Opcode_ & 076) == 032 || _Opcode_ == 067)
			{
				if (loads & 1 << _Rs_)
				{
					loads |= 1 << _Rt_;
					continue;
				}
				else
					reads |= 1 << _Rs_;
				if (reads & 1 << _Rt_)
				{
					s_nBlockFF = false;
					break;
				}
			}
			else if ((_Opcode_ & 074) == 020 && _Rs_ < 4)
			{
				loads |= 1 << _Rt_;
			}
			else
			{
				s_nBlockFF = false;
				break;
			}
		}
	}
	else
	{
		is_timeout_loop = false;
	}

	bool has_cop2_instructions = false;
	{
		if (s_nInstCacheSize < (s_nEndBlock - startpc) / 4 + 1)
		{
			free(s_pInstCache);
			s_nInstCacheSize = (s_nEndBlock - startpc) / 4 + 10;
			s_pInstCache = (EEINST*)malloc(sizeof(EEINST) * s_nInstCacheSize);
			pxAssert(s_pInstCache != NULL);
		}

		EEINST* pcur = s_pInstCache + (s_nEndBlock - startpc) / 4;
		_recClearInst(pcur);
		pcur->info = 0;

		for (i = s_nEndBlock; i > startpc; i -= 4)
		{
			cpuRegs.code = *(int*)PSM(i - 4);
			pcur[-1] = pcur[0];
			recBackpropBSC(cpuRegs.code, pcur - 1, pcur);
			pcur--;

			has_cop2_instructions |= (_Opcode_ == 022 || _Opcode_ == 066 || _Opcode_ == 076);
		}
	}

	if (has_cop2_instructions)
	{
		COP2MicroFinishPass().Run(startpc, s_nEndBlock, s_pInstCache + 1);

		if (EmuConfig.Speedhacks.vuFlagHack)
			COP2FlagHackPass().Run(startpc, s_nEndBlock, s_pInstCache + 1);
	}

#ifdef DUMP_BLOCKS
	ZydisDecoder disas_decoder;
	ZydisDecoderInit(&disas_decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

	ZydisFormatter disas_formatter;
	ZydisFormatterInit(&disas_formatter, ZYDIS_FORMATTER_STYLE_INTEL);

	s_old_print_address = (ZydisFormatterFunc)&ZydisFormatterPrintAddressAbsolute;
	ZydisFormatterSetHook(&disas_formatter, ZYDIS_FORMATTER_FUNC_PRINT_ADDRESS_ABS, (const void**)&s_old_print_address);
	s_old_print_disp = (ZydisFormatterFunc)&ZydisFormatterPrintDisplacement;
	ZydisFormatterSetHook(&disas_formatter, ZYDIS_FORMATTER_FUNC_PRINT_DISP, (const void**)&s_old_print_disp);

#if 0
	const bool dump_block = (startpc == 0x00000000);
#elif 1
	const bool dump_block = true;
#else
	const bool dump_block = false;
#endif
#endif

	memory_protect_recompiled_code(startpc, (s_nEndBlock - startpc) >> 2);

	const bool doRecompilation = !skipMPEG_By_Pattern(startpc) && !recSkipTimeoutLoop(timeout_reg, is_timeout_loop);

	if (doRecompilation)
	{
		g_pCurInstInfo = s_pInstCache;
		while (!g_branch && pc < s_nEndBlock)
		{
#ifdef DUMP_BLOCKS
			if (dump_block)
			{
				std::string disasm;
				disR5900Fasm(disasm, *(u32*)PSM(pc), pc, false);
				fprintf(stderr, "Compiling %08X %s\n", pc, disasm.c_str());

				const u8* inst_start = x86Ptr;
				recompileNextInstruction(false, false);

				const u8* inst_ptr = inst_start;
				ZyanUSize inst_length = static_cast<ZyanUSize>(x86Ptr - inst_start);

				ZydisDecodedInstruction instruction;
				ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];

				while (ZYAN_SUCCESS(ZydisDecoderDecodeFull(&disas_decoder, inst_ptr, inst_length, &instruction, operands)))
				{
					char buffer[256];
					ZyanStatus status = ZydisFormatterFormatInstruction(
						&disas_formatter,
						&instruction,
						operands,
						instruction.operand_count,
						buffer,
						sizeof(buffer),
						reinterpret_cast<ZyanU64>(inst_ptr),
						nullptr);

					if (ZYAN_SUCCESS(status))
						std::fprintf(stderr, "    %016" PRIX64 "    %s\n", reinterpret_cast<u64>(inst_ptr), buffer);

					inst_ptr += instruction.length;
					inst_length -= instruction.length;
				}
			}
			else
			{
				recompileNextInstruction(false, false);
			}
#else
			recompileNextInstruction(false, false);
#endif
		}
	}

	pxAssert((pc - startpc) >> 2 <= 0xffff);
	s_pCurBlockEx->size = (pc - startpc) >> 2;

	if (HWADDR(pc) <= Ps2MemSize::ExposedRam)
	{
		BASEBLOCKEX* oldBlock;
		int i;

		i = recBlocks.LastIndex(HWADDR(pc) - 4);
		while ((oldBlock = recBlocks[i--]))
		{
			if (oldBlock == s_pCurBlockEx)
				continue;
			if (oldBlock->startpc >= HWADDR(pc))
				continue;
			if ((oldBlock->startpc + oldBlock->size * 4) <= HWADDR(startpc))
				break;

			if (memcmp(&recRAMCopy[oldBlock->startpc / 4], PSM(oldBlock->startpc),
					oldBlock->size * 4))
			{
				recClear(startpc, (pc - startpc) / 4);
				s_pCurBlockEx = recBlocks.Get(HWADDR(startpc));
				pxAssert(s_pCurBlockEx->startpc == HWADDR(startpc));
				break;
			}
		}

		memcpy(&recRAMCopy[HWADDR(startpc) / 4], PSM(startpc), pc - startpc);
	}

	s_pCurBlock->SetFnptr((uptr)recPtr);

	if (!(pc & 0x10000000))
		maxrecmem = std::max((pc & ~0xa0000000), maxrecmem);

	if (g_branch == 2)
	{

		iFlushCall(FLUSH_EVERYTHING);
		iBranchTest();
	}
	else
	{
		if (g_branch)
			pxAssert(!willbranch3);

		if (willbranch3 || !g_branch)
		{

			iFlushCall(FLUSH_EVERYTHING);

			const int numinsts = (pc - startpc) / 4;
			if (numinsts > 6)
				SetBranchImm(pc);
			else
			{
				xMOV(ptr32[&cpuRegs.pc], pc);
				xADD(ptr64[&cpuRegs.cycle], scaleblockcycles());
				recBlocks.Link(HWADDR(pc), xJcc32());
			}
		}
	}

	pxAssert(xGetPtr() < SysMemory::GetEERecEnd());

	s_pCurBlockEx->x86size = static_cast<u32>(xGetPtr() - recPtr);

#if 0
	if (startpc == 0x456630) {
		iDumpBlock(s_pCurBlockEx->startpc, s_pCurBlockEx->size*4, s_pCurBlockEx->fnptr, s_pCurBlockEx->x86size);
	}
#endif
	Perf::ee.RegisterPC((void*)s_pCurBlockEx->fnptr, s_pCurBlockEx->x86size, s_pCurBlockEx->startpc);

	recPtr = xGetPtr();

	pxAssert((g_cpuHasConstReg & g_cpuFlushedConstReg) == g_cpuHasConstReg);

	s_pCurBlock = nullptr;
	s_pCurBlockEx = nullptr;
}

R5900cpu recCpu = {
	recReserve,
	recShutdown,

	recResetEE,
	recStep,
	recExecute,

	recSafeExitExecution,
	recCancelInstruction,
	recClear};
