/*
 * Copyright (c) 2015 RISC-V Foundation
 * Copyright (c) 2017 The University of Virginia
 * Copyright (c) 2026 ByteDance
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "arch/riscv/insts/mem.hh"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>

#include "arch/riscv/insts/static_inst.hh"
#include "arch/riscv/memflags.hh"
#include "arch/riscv/regs/vector.hh"
#include "arch/riscv/utility.hh"
#include "cpu/base.hh"
#include "cpu/static_inst.hh"

namespace gem5
{

namespace RiscvISA
{

std::string
Load::generateDisassembly(Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << ' ' << registerName(destRegIdx(0)) << ", " <<
        offset << '(' << registerName(srcRegIdx(0)) << ')';
    return ss.str();
}

std::string
Store::generateDisassembly(Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    if (_numSrcRegs == 1) {
        ss << mnemonic << ' ' << offset << '(' << registerName(srcRegIdx(0))
           << ")";
    } else {
        ss << mnemonic << ' ' << registerName(srcRegIdx(1)) << ", " <<
            offset << '(' << registerName(srcRegIdx(0)) << ')';
    }
    return ss.str();
}

namespace
{

/**
 * A descriptor consists of the architectural cursors/count and one scalar
 * beat descriptor. Data uses one native vector register. Pointer updates,
 * count update, and loop control consume separate integer micro-ops.
 */
class ZimemopsMicro : public RiscvMicroInst
{
  public:
    enum Step
    {
        Setup,
        Read,
        Write,
        Dest,
        Source,
        Count,
        Loop,
        End
    };

  private:
    static constexpr RegVal Reverse = 1ULL << 9;
    const Step step;
    const bool fill;
    const bool move;
    const MicroPC branchDest;
    const unsigned maxBytes;
    const unsigned beatBytes;
    const bool second;
    RegId srcRegs[6];
    RegId destRegs[1];

    RegVal
    operand(ExecContext *xc, int index) const
    {
        return rvZext(xc->getRegOperand(this, index));
    }

    unsigned
    setupSize(ExecContext *xc, bool reverse) const
    {
        uint64_t size = std::min<uint64_t>(
            operand(xc, 2), std::min(maxBytes, xc->maxMemAccessSize()));
        for (int index = 0; index < (fill ? 1 : 2); ++index) {
            const Addr base = operand(xc, index);
            const Addr cursor = reverse ? rvZext(base + operand(xc, 2)) : base;
            const uint64_t page_left =
                reverse ? ((cursor - 1) & 4095) + 1 : 4096 - (cursor & 4095);
            size = std::min(size, page_left);
        }
        // Keep a two-beat group whole; short/page-edge groups use one beat.
        return size >= 2 * beatBytes ? 2 * beatBytes
                                     : std::min<uint64_t>(size, beatBytes);
    }

    RegVal
    descriptor(ExecContext *xc) const
    {
        return operand(xc, step == Read || step == Write ? 3 : 1);
    }

    unsigned
    amount(ExecContext *xc) const
    {
        const unsigned total = descriptor(xc) & (Reverse - 1);
        return second ? (total > beatBytes ? total - beatBytes : 0)
                      : std::min(total, beatBytes);
    }

    Addr
    address(ExecContext *xc, int index) const
    {
        const RegVal meta = descriptor(xc);
        const unsigned total = meta & (Reverse - 1);
        if (step == Read) {
            return rvSext(operand(xc, index) +
                          ((meta & Reverse) ? operand(xc, 2) - total : 0) +
                          (second ? beatBytes : 0));
        }
        return rvSext(operand(xc, index) +
                      ((meta & Reverse) ? operand(xc, 2) - amount(xc) : 0));
    }

    Request::Flags
    memoryFlags() const
    {
        return XlateFlags::ZIMEMOPS |
               (maxBytes > 1 ? XlateFlags::ZIMEMOPS_WIDE : 0);
    }

    Fault
    write(ExecContext *xc) const
    {
        const unsigned size = amount(xc);
        if (!size) {
            xc->setMemAccPredicate(false);
            return NoFault;
        }
        VecRegContainer data;
        if (fill) {
            std::memset(data.as<uint8_t>(), operand(xc, 1) & 255, size);
        } else {
            const bool grouped = (descriptor(xc) & (Reverse - 1)) > beatBytes;
            const bool high =
                grouped && ((descriptor(xc) & Reverse) ? !second : second);
            xc->getRegOperand(this, high ? 5 : 4, &data);
        }
        return xc->writeMem(data.as<uint8_t>(), size, address(xc, 0),
                            memoryFlags(), nullptr,
                            std::vector<bool>(size, true));
    }

  public:
    ZimemopsMicro(const char *mnem, ExtMachInst inst, Step step,
                  MicroPC target = 0, unsigned bytes = 1,
                  unsigned beat_bytes = 1, bool second_beat = false)
        : RiscvMicroInst(mnem, inst,
                         step == Read
                             ? (bytes > 1 ? SimdUnitStrideLoadOp : MemReadOp)
                         : step == Write
                             ? (bytes > 1 ? SimdUnitStrideStoreOp : MemWriteOp)
                             : IntAluOp),
          step(step),
          fill(inst.funct7 == 6),
          move(inst.funct7 == 3),
          branchDest(target),
          maxBytes(bytes),
          beatBytes(beat_bytes),
          second(second_beat)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(&ZimemopsMicro::srcRegs),
            reinterpret_cast<RegIdArrayPtr>(&ZimemopsMicro::destRegs));
        flags[IsInteger] = true;
        const RegId meta = intRegClass[int_reg::_Ureg0Idx];
        if (step == Setup || step == Read || step == Write) {
            setSrcRegIdx(_numSrcRegs++, intRegClass[inst.rd]);
            setSrcRegIdx(_numSrcRegs++, intRegClass[inst.rs1]);
            setSrcRegIdx(_numSrcRegs++, intRegClass[inst.rs2]);
            if (step != Setup) {
                setSrcRegIdx(_numSrcRegs++, meta);
            }
        } else if (step != End) {
            const RegIndex index = step == Dest     ? inst.rd
                                   : step == Source ? inst.rs1
                                                    : inst.rs2;
            setSrcRegIdx(_numSrcRegs++, intRegClass[index]);
            if (step != Loop) {
                setSrcRegIdx(_numSrcRegs++, meta);
            }
        }
        if (step == Setup) {
            setDestRegIdx(_numDestRegs++, meta);
            _numTypedDestRegs[IntRegClass] = 1;
        } else if (step == Read) {
            flags[IsLoad] = true;
            setDestRegIdx(_numDestRegs++,
                          vecRegClass[VecMemInternalReg0 + second]);
            _numTypedDestRegs[VecRegClass] = 1;
        } else if (step == Write) {
            flags[IsStore] = true;
            if (!fill) {
                setSrcRegIdx(_numSrcRegs++, vecRegClass[VecMemInternalReg0]);
                if (maxBytes > beatBytes) {
                    setSrcRegIdx(_numSrcRegs++,
                                 vecRegClass[VecMemInternalReg0 + 1]);
                }
            }
        } else if (step == Dest || step == Source || step == Count) {
            setDestRegIdx(_numDestRegs++, srcRegIdx(0));
            _numTypedDestRegs[IntRegClass] = 1;
        } else if (step == Loop) {
            flags[IsControl] = true;
            flags[IsDirectControl] = true;
            flags[IsCondControl] = true;
        } else {
            flags[IsLastMicroop] = true;
        }
        // A store and all its progress updates form one interrupt boundary.
        flags[IsDelayedCommit] = step != Count && step != Loop && step != End;
    }

    Fault
    execute(ExecContext *xc, trace::InstRecord *) const override
    {
        if (step == Setup) {
            const bool reverse = move && operand(xc, 0) > operand(xc, 1);
            xc->setRegOperand(
                this, 0, setupSize(xc, reverse) | (reverse ? Reverse : 0));
        } else if (step == Read) {
            VecRegContainer data;
            data.zero();
            const unsigned size = amount(xc);
            if (size) {
                Fault fault =
                    xc->readMem(address(xc, 1), data.as<uint8_t>(), size,
                                memoryFlags(), std::vector<bool>(size, true));
                if (fault != NoFault) {
                    return fault;
                }
            }
            xc->setRegOperand(this, 0, &data);
        } else if (step == Write) {
            return write(xc);
        } else if (step == Dest || step == Source) {
            const unsigned delta = descriptor(xc) & Reverse ? 0 : amount(xc);
            xc->setRegOperand(this, 0, rvSext(operand(xc, 0) + delta));
        } else if (step == Count) {
            xc->setRegOperand(this, 0, rvSext(operand(xc, 0) - amount(xc)));
        } else if (step == Loop && operand(xc, 0)) {
            PCState pc = xc->pcState().as<PCState>();
            pc.nupc(branchDest);
            xc->pcState(pc);
        }
        return NoFault;
    }

    Fault
    initiateAcc(ExecContext *xc, trace::InstRecord *) const override
    {
        if (step == Write) {
            return write(xc);
        }
        assert(step == Read);
        const unsigned size = amount(xc);
        if (!size) {
            xc->setMemAccPredicate(false);
            return NoFault;
        }
        return xc->initiateMemRead(address(xc, 1), size, memoryFlags(),
                                   std::vector<bool>(size, true));
    }

    Fault
    completeAcc(PacketPtr pkt, ExecContext *xc,
                trace::InstRecord *) const override
    {
        if (step == Read) {
            VecRegContainer data;
            data.zero();
            if (pkt) {
                std::memcpy(data.as<uint8_t>(), pkt->getConstPtr<uint8_t>(),
                            pkt->getSize());
            }
            xc->setRegOperand(this, 0, &data);
        }
        return NoFault;
    }

    std::unique_ptr<PCStateBase>
    branchTarget(const PCStateBase &pc) const override
    {
        auto target = std::make_unique<PCState>(pc.as<PCState>());
        target->nupc(branchDest);
        target->uAdvance();
        return target;
    }

    std::string
    generateDisassembly(Addr, const loader::SymbolTable *) const override
    {
        static const char *names[] = {"setup",  "load",  "store", "dest",
                                      "source", "count", "loop",  "end"};
        return std::string(mnemonic) + "." + names[step];
    }
};

/** Fixed ROM bodies selected by an effective byte length, like RVV's VL. */
struct ZimemopsLengths
{
    unsigned maxBytes;
    unsigned tailBytes;
    unsigned maxCommands;
    unsigned beatBytes;
    unsigned scalarThreshold;
    unsigned wordBytes;
    bool fill;
    bool move;
    MicroPC zero = 0;
    MicroPC scalar = 0;
    MicroPC vector = 0;
    MicroPC doubleHead = 0;
    MicroPC deferredDoubleHead = 0;
    MicroPC ownedDoubleHead = 0;
    MicroPC ownedDoubleEdges = 0;
    MicroPC fiveCommandTail = 0;
    std::array<MicroPC, 6> ownedTails = {};
    std::array<MicroPC, 6> ownedLineTails = {};
    std::array<MicroPC, 5> loops = {};
    std::array<MicroPC, 5> finals = {};
    std::array<MicroPC, 5> ownedLoops = {};
    std::array<MicroPC, 5> bareHeads = {};
    std::array<MicroPC, 5> pairedLoops = {};
    std::array<MicroPC, 5> pairedFinals = {};
    std::array<MicroPC, 5> pairedEntries = {};
    std::array<MicroPC, 5> pairedEntryFinals = {};
    MicroPC pairedFiveTail = 0;
    MicroPC deferredFiveTail = 0;
    std::array<MicroPC, 5> deferredLoops = {};
    std::array<MicroPC, 5> deferredFinals = {};
    std::array<MicroPC, 5> deferredEntries = {};
    std::array<MicroPC, 5> deferredEntryFinals = {};
    unsigned longBytes = 0;
    MicroPC longLoop = 0;
    MicroPC longFinal = 0;
    MicroPC longEntry = 0;
    MicroPC longEntryFinal = 0;

    MicroPC
    loopTarget(unsigned count, unsigned stride, bool deferred = false) const
    {
        if (count > maxCommands) {
            return longLoop;
        }
        if (deferred) {
            return deferredLoops[count];
        }
        return stride == 2 ? pairedLoops[count] : loops[count];
    }

    bool
    isOwnedHead(MicroPC pc) const
    {
        for (unsigned count = 1; count <= maxCommands; ++count) {
            if (ownedLoops[count] == pc || bareHeads[count] == pc) {
                return true;
            }
        }
        return false;
    }

    bool
    isFirstWindow(MicroPC pc) const
    {
        if (longBytes && (pc == longEntry || pc == longEntryFinal)) {
            return true;
        }
        if (!move && pc == doubleHead) {
            return true;
        }
        if (deferredDoubleHead && pc == deferredDoubleHead) {
            return true;
        }
        for (unsigned count = 2; count <= maxCommands; ++count) {
            if (pc == pairedEntries[count] || pc == pairedEntryFinals[count]) {
                return true;
            }
        }
        return false;
    }

    unsigned
    windowCommands(RegVal original) const
    {
        return original >= 4096 ? std::min(2U, maxCommands) : maxCommands;
    }

    unsigned
    bytes(RegVal d, RegVal s, RegVal original, RegVal remaining,
          unsigned width, unsigned limit, bool align = true,
          unsigned span = 0) const
    {
        uint64_t size =
            std::min<uint64_t>(remaining, std::min(span    ? span
                                                   : align ? maxBytes
                                                           : tailBytes,
                                                   limit));
        const bool back = move && d > s;
        const unsigned delta = (original - remaining) & 4095;
        const unsigned alignment = 2 * beatBytes;
        for (const RegVal base : {d, s}) {
            const unsigned cursor = (base + (back ? remaining : delta)) & 4095;
            if (base == d && align) {
                const unsigned fragment =
                    back ? ((cursor - 1) & (alignment - 1)) + 1
                         : alignment - (cursor & (alignment - 1));
                if (cursor & (alignment - 1)) {
                    size = std::min<uint64_t>(size, fragment);
                }
            }
            // Aligned power-of-two commands may cross a page between beats.
            // An unaligned terminal destination must not split a store at
            // the page edge: its whole operation was checked at entry.
            if ((!align && base == d && (cursor & (width - 1))) ||
                (width & (width - 1))) {
                const unsigned left =
                    back ? ((cursor - 1) & 4095) + 1 : 4096 - cursor;
                size = std::min<uint64_t>(size, left);
            }
            if (fill) {
                break;
            }
        }
        return size;
    }

    MicroPC
    select(RegVal d, RegVal s, RegVal original, RegVal remaining,
           unsigned width, bool entry) const
    {
        if (entry && !remaining) {
            return zero;
        }
        bool short_command = entry && remaining <= width;
        if (short_command) {
            const bool back = move && d > s;
            for (const RegVal base : {d, s}) {
                const unsigned cursor = (base + (back ? remaining : 0)) & 4095;
                const unsigned left =
                    back ? ((cursor - 1) & 4095) + 1 : 4096 - cursor;
                if (remaining > left) {
                    short_command = false;
                }
                if (fill) {
                    break;
                }
            }
        }
        if (short_command && move && d != s) {
            const RegVal distance = d > s ? d - s : s - d;
            if (distance < remaining &&
                bytes(d, s, original, remaining, width, width) != remaining) {
                short_command = false;
            }
            if (distance < remaining) {
                const bool back = d > s;
                for (const RegVal base : {d, s}) {
                    const unsigned cursor =
                        (base + (back ? remaining : 0)) & 4095;
                    const unsigned left =
                        back ? ((cursor - 1) & 4095) + 1 : 4096 - cursor;
                    if (remaining > left) {
                        short_command = false;
                    }
                }
            }
        }
        if (short_command) {
            if (scalarThreshold &&
                remaining <= std::min({scalarThreshold, wordBytes, width})) {
                return scalar;
            }
            return vector;
        }
        const RegVal distance = d > s ? d - s : s - d;
        const bool overlap = move && distance && distance < remaining;
        const bool back = move && d > s;
        const unsigned alignment = 2 * beatBytes;
        const unsigned first = (d + (back ? remaining : 0)) & (alignment - 1);
        const bool first_fits =
            back ? !first || first >= width : alignment - first >= width;
        if (entry && fiveCommandTail && !move && !(width & (width - 1)) &&
            !(d & (width - 1)) && remaining > 4 * width &&
            remaining <= 5 * width && remaining <= tailBytes &&
            bytes(d, s, original, remaining, width, 5 * width, false) ==
                remaining) {
            if (!(d & (alignment - 1))) {
                return deferredFiveTail && !(s & (alignment - 1))
                           ? deferredFiveTail
                           : pairedFiveTail;
            }
            return fiveCommandTail;
        }
        if (entry && !move && !(width & (width - 1)) &&
            (d & (alignment - 1)) && remaining > 2 * width &&
            remaining <= std::min(maxBytes, maxCommands * width) &&
            bytes(d, s, original, remaining, width, maxBytes, false) ==
                remaining) {
            // Fold the alignment head into this complete operation. A
            // two-command head batch ends at a cache-line boundary in the
            // first half; three commands do so in the second half.
            const unsigned count =
                (remaining + (d & (width - 1)) + width - 1) / width;
            return first >= width ? ownedLineTails[count] : ownedTails[count];
        }
        if (entry && doubleHead && !overlap &&
            remaining <= std::min(maxBytes, 2 * width) &&
            bytes(d, s, original, remaining, width, 2 * width, false) ==
                remaining) {
            if ((d & (alignment - 1)) || remaining < alignment) {
                if (!first_fits &&
                    ((d + remaining - 1) & ~(RegVal(alignment) - 1)) !=
                        (d & ~(RegVal(alignment) - 1))) {
                    return ownedDoubleEdges;
                }
                return ownedDoubleHead;
            }
            if (first_fits) {
                if (deferredDoubleHead && !fill && !move &&
                    width == beatBytes && !(width & (width - 1)) &&
                    !(s & (alignment - 1))) {
                    return deferredDoubleHead;
                }
                return doubleHead;
            }
        }
        const unsigned size = bytes(d, s, original, remaining, width,
                                    windowCommands(original) * width);
        const unsigned count = std::max(1U, (size + width - 1) / width);
        const RegVal cursor = d + original - remaining;
        const RegVal source_cursor = s + original - remaining;
        if (!fill && !move && longBytes && width == beatBytes &&
            !((cursor | source_cursor) & (2 * width - 1)) &&
            remaining >= longBytes) {
            if (entry) {
                return remaining == longBytes ? longEntryFinal : longEntry;
            }
            return remaining == longBytes ? longFinal : longLoop;
        }
        if (!fill && !move && width == beatBytes && !(width & (width - 1)) &&
            count >= 2 && !((cursor | source_cursor) & (2 * width - 1))) {
            if (entry) {
                return size == remaining ? deferredEntryFinals[count]
                                         : deferredEntries[count];
            }
            return size == remaining ? deferredFinals[count]
                                     : deferredLoops[count];
        }
        if (!move && width == beatBytes && !(width & (width - 1)) &&
            count >= 2 && !(cursor & (2 * width - 1))) {
            if (entry) {
                return size == remaining ? pairedEntryFinals[count]
                                         : pairedEntries[count];
            }
            return size == remaining ? pairedFinals[count]
                                     : pairedLoops[count];
        }
        if (entry && !move && (d & (alignment - 1)) && size < remaining) {
            if (!fill && longBytes && width == beatBytes &&
                !(width & (width - 1)) && remaining - size >= longBytes &&
                !((s - d) & (alignment - 1))) {
                return bareHeads[count];
            }
            if (count == 1 && original > tailBytes) {
                return bareHeads[count];
            }
            return ownedLoops[count];
        }
        return size == remaining ? finals[count] : loops[count];
    }
};

/** An ordinary ownership hint for a partial destination cache line. */
class ZimemopsPrefetch : public RiscvMicroInst
{
  private:
    static constexpr Request::FlagsType PrefetchFlags =
        Request::FlagsType(Request::PF_EXCLUSIVE) | XlateFlags::ZIMEMOPS;
    const bool last;
    RegId srcRegs[2];
    RegId destRegs[1];

    bool
    active(ExecContext *xc) const
    {
        return rvZext(xc->getRegOperand(this, 1)) != 0;
    }

    Addr
    address(ExecContext *xc) const
    {
        const RegVal d = rvZext(xc->getRegOperand(this, 0));
        const RegVal r = rvZext(xc->getRegOperand(this, 1));
        return rvSext(d + (last ? r - 1 : 0));
    }

  public:
    ZimemopsPrefetch(const char *mnem, ExtMachInst inst, bool last = false)
        : RiscvMicroInst(mnem, inst, MemReadOp), last(last)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(&ZimemopsPrefetch::srcRegs),
            reinterpret_cast<RegIdArrayPtr>(&ZimemopsPrefetch::destRegs));
        setSrcRegIdx(_numSrcRegs++, intRegClass[inst.rd]);
        setSrcRegIdx(_numSrcRegs++, intRegClass[inst.rs2]);
        flags[IsInteger] = true;
        flags[IsLoad] = true;
        flags[IsDataPrefetch] = true;
        flags[IsDelayedCommit] = true;
    }

    Fault
    execute(ExecContext *xc, trace::InstRecord *) const override
    {
        if (active(xc)) {
            uint8_t data;
            xc->readMem(address(xc), &data, 1, PrefetchFlags,
                        std::vector<bool>(1, true));
        } else {
            xc->setMemAccPredicate(false);
        }
        return NoFault;
    }

    Fault
    initiateAcc(ExecContext *xc, trace::InstRecord *) const override
    {
        if (!active(xc)) {
            xc->setMemAccPredicate(false);
            return NoFault;
        }
        return xc->initiateMemRead(address(xc), 1, PrefetchFlags,
                                   std::vector<bool>(1, true));
    }

    Fault
    completeAcc(PacketPtr, ExecContext *, trace::InstRecord *) const override
    {
        return NoFault;
    }

    void
    synchronizeState(ThreadContext *tc) const override
    {
        const MicroPC pc = tc->pcState().microPC();
        if (!last || pc < 1 || pc >= ZimemopsBytePC) {
            return;
        }
        const RegVal r = rvZext(tc->getReg(intRegClass[machInst.rs2]));
        const RegVal issued =
            rvZext(tc->getReg(intRegClass[int_reg::_StreamIssueIdx]));
        if (r <= issued) {
            return;
        }
        // Tail hints precede this terminal body's first store. Only the
        // already completed prefix is visible at a stop. Rebasing both
        // pointers and the count preserves every in-flight address.
        const RegVal d = rvZext(tc->getReg(intRegClass[machInst.rd]));
        const RegVal s = rvZext(tc->getReg(intRegClass[machInst.rs1]));
        const bool fill = machInst.funct7 == 6;
        const bool back = machInst.funct7 == 3 && d > s;
        const RegVal done = back ? 0 : r - issued;
        if (!fill) {
            tc->setReg(intRegClass[machInst.rs1], rvSext(s + done));
        }
        tc->setReg(intRegClass[machInst.rd], rvSext(d + done));
        tc->setReg(intRegClass[machInst.rs2], rvSext(issued));
    }

    std::string
    generateDisassembly(Addr, const loader::SymbolTable *) const override
    {
        return std::string(mnemonic) + ".prefetch_w";
    }
};

/** Streaming descriptor: 16 B of scalar state, native-register data beats. */
class ZimemopsStream : public RiscvMicroInst
{
  public:
    enum Step
    {
        InitIssue,
        Issue,
        Continue,
        Select,
        Read,
        Write,
        BatchEnd,
        PublishD,
        PublishS,
        PublishR,
        Zero
    };

  private:
    const Step step;
    const unsigned groupBytes;
    const unsigned beatBytes;
    const unsigned commands;
    const unsigned commandBytes;
    const unsigned ordinal;
    const MicroPC target;
    const MicroPC publishPC;
    const MicroPC firstStorePC;
    const unsigned controlWidth;
    const unsigned tempRegs;
    const unsigned scalarThreshold;
    const MicroPC shortTarget;
    const bool scalar;
    const bool terminal;
    const bool initialRead;
    const bool finalCount;
    const bool peeledTail;
    const unsigned storeStride;
    const bool endCursor;
    const bool firstAligned;
    const bool deferredPeer;
    const std::shared_ptr<const ZimemopsLengths> lengths;
    const bool fill;
    const bool move;
    RegId srcRegs[6];
    RegId destRegs[1];

    RegVal
    value(ExecContext *xc, unsigned i) const
    {
        return rvZext(xc->getRegOperand(this, i));
    }

    RegVal
    remainingBytes(ExecContext *xc) const
    {
        // The legacy single-command terminal body needs no cursor dependency.
        return value(xc, initialRead || (terminal && !lengths) ? 2 : 3);
    }

    bool
    reverse(ExecContext *xc) const
    {
        return move && value(xc, 0) > value(xc, 1);
    }

    unsigned
    beatSize(ExecContext *xc) const
    {
        return lengths ? std::min(
                             {commandBytes, xc->maxMemAccessSize(),
                              unsigned(
                                  xc->tcBase()->getCpuPtr()->cacheLineSize())})
                       : std::min(commandBytes, xc->maxMemAccessSize());
    }

    unsigned
    groupSize(ExecContext *xc) const
    {
        const RegVal remaining = remainingBytes(xc);
        const unsigned beat = beatSize(xc);
        if (initialRead) {
            if (lengths && !terminal && !firstAligned) {
                return lengths->bytes(value(xc, 0), value(xc, 1), remaining,
                                      remaining, beat, commands * beat);
            }
            // The first beat of a complete operation precedes every store.
            // Its address starts at the original pointer, and its exact
            // size needs no dependency on the private issue-cursor result.
            return std::min<uint64_t>(remaining,
                                      std::min(groupBytes, commands * beat));
        }
        if (endCursor) {
            const unsigned tail =
                ((value(xc, 2) - remaining - 1) & (beat - 1)) + 1;
            return (commands - 1) * beat + tail;
        }
        if (lengths) {
            return lengths->bytes(value(xc, 0), value(xc, 1), value(xc, 2),
                                  remaining, beat, commands * beat, !terminal,
                                  commands > lengths->maxCommands ? groupBytes
                                                                  : 0);
        }
        uint64_t size = std::min<uint64_t>(
            remaining, std::min(groupBytes, commands * beat));
        const bool back = reverse(xc);
        const RegVal separation =
            back ? value(xc, 0) - value(xc, 1) : value(xc, 1) - value(xc, 0);
        const bool overlap = move && separation && separation < remaining;
        // Boundary logic needs only page offsets; address formation is in LSU.
        const unsigned delta = (value(xc, 2) - remaining) & 4095;
        for (unsigned i = 0; i < (fill ? 1U : 2U); ++i) {
            const unsigned cursor =
                (value(xc, i) + (back ? remaining : delta)) & 4095;
            // Power-of-two native beats cannot cross a destination page
            // after alignment peeling. Loads may use ordinary split
            // translation, with byte replay on a fault. Terminal commands
            // and irregular vector beat widths retain both page caps.
            if (terminal || (beat & (beat - 1))) {
                const unsigned left =
                    back ? ((cursor - 1) & 4095) + 1 : 4096 - cursor;
                size = std::min<uint64_t>(size, left);
            }
            if (i == 0 && (!terminal || overlap)) {
                // Peel a destination fragment before streaming aligned
                // groups. Overlapping terminal commands also peel to avoid
                // partial forwarding across a split store on the next move.
                const unsigned alignment = 2 * beatBytes;
                const unsigned fragment =
                    back ? ((cursor - 1) & (alignment - 1)) + 1
                         : alignment - (cursor & (alignment - 1));
                if (cursor & (alignment - 1)) {
                    size = std::min<uint64_t>(size, fragment);
                }
            }
        }
        // Integer data commands use the existing byte enables for an exact
        // count within one GPR (including 3/5/6/7-byte tails on RV64).
        return size;
    }

    MicroPC
    entryTarget(RegVal remaining, RegVal d = 0, RegVal s = 0) const
    {
        if (move && remaining <= commandBytes) {
            const bool back = d > s;
            const RegVal separation = back ? d - s : s - d;
            if (separation && separation < remaining) {
                const unsigned cursor = (d + (back ? remaining : 0)) & 4095;
                const unsigned alignment = 2 * beatBytes;
                const unsigned fragment =
                    back ? ((cursor - 1) & (alignment - 1)) + 1
                         : alignment - (cursor & (alignment - 1));
                if (remaining > fragment) {
                    return target;
                }
                for (const RegVal base : {d, s}) {
                    const unsigned end =
                        (base + (back ? remaining : 0)) & 4095;
                    const unsigned left =
                        back ? ((end - 1) & 4095) + 1 : 4096 - end;
                    if (remaining > left) {
                        return target;
                    }
                }
            }
        }
        if (scalarThreshold &&
            remaining <=
                std::min({scalarThreshold, rvSelect(4U, 8U), commandBytes})) {
            return 1;
        }
        return remaining <= commandBytes ? shortTarget : target;
    }

    bool
    controlPoint() const
    {
        if (lengths) {
            return false;
        }
        return step == BatchEnd ||
               (step == Write &&
                (terminal || (ordinal + 1) % controlWidth == 0));
    }

    bool
    terminalControl() const
    {
        return step == InitIssue || step == Continue || step == Select ||
               step == BatchEnd ||
               (!terminal && step == Write && ordinal + 1 == commands &&
                controlPoint());
    }

    unsigned
    commandOffset(ExecContext *xc, unsigned index) const
    {
        const unsigned beat = beatSize(xc);
        if (!peeledTail || !index) {
            return index * beat;
        }
        // The issue cursor identifies this body's start even after a stop
        // publishes a prefix and rebases the architectural pointers/count.
        const RegVal cursor = value(xc, 0) + value(xc, 2) - remainingBytes(xc);
        return beat - (cursor & (beat - 1)) + (index - 1) * beat;
    }

    unsigned
    amount(ExecContext *xc) const
    {
        if (endCursor) {
            const unsigned beat = beatSize(xc);
            const RegVal head = initialRead
                                    ? groupSize(xc)
                                    : value(xc, 2) - remainingBytes(xc);
            if (!head) {
                return 0;
            }
            if (!((value(xc, 0) + head) & (beat - 1))) {
                if (ordinal && head < (commands - ordinal) * beat) {
                    return 0;
                }
                return ordinal ? beat : ((head - 1) & (beat - 1)) + 1;
            }
        }
        const unsigned size = groupSize(xc);
        const unsigned offset = commandOffset(xc, ordinal);
        const unsigned width = (step == Write ? storeStride : 1) *
                               (commandOffset(xc, ordinal + 1) - offset);
        return offset < size ? std::min(width, size - offset) : 0;
    }

    Addr
    address(ExecContext *xc) const
    {
        const unsigned size = amount(xc);
        const bool back = reverse(xc);
        const unsigned offset = commandOffset(xc, ordinal);
        const RegVal remaining = remainingBytes(xc);
        if (endCursor && !initialRead) {
            const RegVal head = value(xc, 2) - remaining;
            if (!((value(xc, 0) + head) & (beatSize(xc) - 1))) {
                return rvSext(value(xc, step == Read ? 1 : 0) +
                              (ordinal
                                   ? head - (commands - ordinal) * beatSize(xc)
                                   : 0));
            }
            const RegVal full =
                (value(xc, 2) - remaining - 1) & ~(RegVal(beatSize(xc)) - 1);
            return rvSext(value(xc, step == Read ? 1 : 0) + full +
                          (RegVal(ordinal) - (commands - 1)) * beatSize(xc));
        }
        return rvSext(value(xc, step == Read ? 1 : 0) +
                      (back ? remaining - offset - size
                            : value(xc, 2) - remaining + offset));
    }

    void
    progress(ExecContext *xc, bool redirect) const
    {
        if (finalCount) {
            // This entry body completes in one native store. Use its one
            // result for the architectural count instead of a private count
            // followed by a separate clear. The issue cursor retains the
            // original endpoint length for the two pointer publications.
            xc->setRegOperand(this, 0, RegVal(0));
            return;
        }
        if (lengths) {
            // Default stores have no register result. The committed microPC
            // and fixed beat ordinal identify progress at a stop or fault.
            return;
        }
        const unsigned end = ordinal * beatSize(xc) + amount(xc);
        const unsigned size = groupSize(xc);
        const RegVal next = remainingBytes(xc) - std::min(end, size);
        // Derive progress from this group's immutable issue cursor. Stores
        // need no dependency on a preceding store's result: in-order commit
        // publishes the matching private counter at each progress point.
        // Simple timing waits for the successful memory response instead.
        xc->setRegOperand(this, 0, rvSext(next));
        if (redirect && end >= size && (!terminal || next != 0)) {
            PCState pc = xc->pcState().as<PCState>();
            pc.nupc(target);
            xc->pcState(pc);
        }
    }

    Fault
    write(ExecContext *xc, bool atomic) const
    {
        const unsigned size = amount(xc);
        if (size) {
            Fault fault;
            if (scalar) {
                uint8_t data[sizeof(RegVal)];
                const RegVal word =
                    fill
                        ? value(xc, 1)
                        : value(xc,
                                initialRead || (terminal && !lengths) ? 3 : 4);
                for (unsigned i = 0; i < size; ++i) {
                    data[i] = (word >> (fill ? 0 : 8 * i)) & 255;
                }
                fault = xc->writeMem(data, size, address(xc),
                                     XlateFlags::ZIMEMOPS |
                                         XlateFlags::ZIMEMOPS_WIDE,
                                     nullptr, std::vector<bool>(size, true));
            } else {
                VecRegContainer data;
                if (fill) {
                    std::memset(data.as<uint8_t>(), value(xc, 1) & 255, size);
                } else {
                    xc->getRegOperand(
                        this, initialRead || (terminal && !lengths) ? 3 : 4,
                        &data);
                    if (storeStride == 2 && size > beatSize(xc)) {
                        VecRegContainer second;
                        xc->getRegOperand(this, 5, &second);
                        std::memcpy(data.as<uint8_t>() + beatSize(xc),
                                    second.as<uint8_t>(), size - beatSize(xc));
                    }
                }
                fault = xc->writeMem(data.as<uint8_t>(), size, address(xc),
                                     XlateFlags::ZIMEMOPS |
                                         XlateFlags::ZIMEMOPS_WIDE,
                                     nullptr, std::vector<bool>(size, true));
            }
            if (fault != NoFault) {
                return fault;
            }
        } else {
            xc->setMemAccPredicate(false);
        }
        if (atomic || !size || xc->hasRegisterRenaming()) {
            progress(xc, controlPoint());
        }
        return NoFault;
    }

    void
    loadResult(ExecContext *xc, const uint8_t *data, unsigned size) const
    {
        if (scalar) {
            RegVal word = 0;
            for (unsigned i = 0; i < size; ++i) {
                word |= RegVal(data[i]) << (8 * i);
            }
            xc->setRegOperand(this, 0, rvSext(word));
        } else {
            VecRegContainer word;
            word.zero();
            if (size) {
                std::memcpy(word.as<uint8_t>(), data, size);
            }
            xc->setRegOperand(this, 0, &word);
        }
    }

  public:
    ZimemopsStream(
        const char *mnem, ExtMachInst inst, Step step, unsigned group,
        unsigned beat, unsigned commands, unsigned command_bytes,
        unsigned ordinal = 0, MicroPC target = 1, MicroPC publish_pc = 1,
        MicroPC first_store_pc = 1, unsigned control_width = 2,
        unsigned temp_regs = 2, bool scalar = false,
        unsigned scalar_threshold = 0, MicroPC short_target = 1,
        bool terminal = false,
        std::shared_ptr<const ZimemopsLengths> active_lengths = nullptr,
        bool initial_read = false, bool final_count = false,
        bool peeled_tail = false, unsigned store_stride = 1,
        bool end_cursor = false, bool first_aligned = false,
        bool deferred_peer = false)
        : RiscvMicroInst(
              mnem, inst,
              step == Zero    ? No_OpClass
              : step == Read  ? (scalar ? MemReadOp : SimdUnitStrideLoadOp)
              : step == Write ? (scalar ? MemWriteOp : SimdUnitStrideStoreOp)
                              : IntAluOp),
          step(step),
          groupBytes(group),
          beatBytes(beat),
          commands(commands),
          commandBytes(command_bytes),
          ordinal(ordinal),
          target(target),
          publishPC(publish_pc),
          firstStorePC(first_store_pc),
          controlWidth(control_width),
          tempRegs(temp_regs),
          scalarThreshold(scalar_threshold),
          shortTarget(short_target),
          scalar(scalar),
          terminal(scalar || terminal),
          initialRead(initial_read),
          finalCount(final_count),
          peeledTail(peeled_tail),
          storeStride(store_stride),
          endCursor(end_cursor),
          firstAligned(first_aligned),
          deferredPeer(deferred_peer),
          lengths(std::move(active_lengths)),
          fill(inst.funct7 == 6),
          move(inst.funct7 == 3)
    {
        setRegIdxArrays(
            reinterpret_cast<RegIdArrayPtr>(&ZimemopsStream::srcRegs),
            reinterpret_cast<RegIdArrayPtr>(&ZimemopsStream::destRegs));
        flags[IsInteger] = true;
        auto src = [&](RegIndex reg) {
            setSrcRegIdx(_numSrcRegs++, intRegClass[reg]);
        };
        auto dst = [&](RegIndex reg) {
            setDestRegIdx(_numDestRegs++, intRegClass[reg]);
            _numTypedDestRegs[IntRegClass] = 1;
        };
        if (step == InitIssue) {
            if (lengths) {
                src(inst.rd);
                src(fill ? 0 : inst.rs1);
            } else if (move) {
                src(inst.rd);
                src(inst.rs1);
            }
            src(inst.rs2);
            dst(int_reg::_StreamIssueIdx);
        } else if (step == Continue) {
            if (lengths) {
                src(inst.rd);
                src(move || (!fill && (commandBytes & (commandBytes - 1)))
                        ? inst.rs1
                        : 0);
                src(inst.rs2);
            }
            src(int_reg::_StreamRemainingIdx);
            dst(int_reg::_StreamIssueIdx);
        } else if (step == Zero) {
            // Count zero preserves every architectural register.
        } else if (step == PublishR) {
            // Both completion paths have zero remaining bytes. This ordinary
            // one-result clear need not wait for the pointer publications.
            dst(inst.rs2);
        } else if (!move && (step == PublishD || step == PublishS)) {
            // Copies and fills have a fixed forward direction. Publishing
            // either pointer need not wait for the other pointer's result
            // or the store's count result: this footer is reached only at
            // zero remaining bytes. In-order retirement preserves faults.
            src(step == PublishD ? inst.rd : inst.rs1);
            src(finalCount ? int_reg::_StreamIssueIdx : inst.rs2);
            dst(step == PublishD ? inst.rd : inst.rs1);
        } else {
            src(step == Read && !move && !peeledTail &&
                        (!endCursor || firstAligned) &&
                        ((initialRead && (this->terminal || firstAligned)) ||
                         (this->terminal && !lengths))
                    ? 0
                    : inst.rd);
            // Native forward Issue checks only destination alignment. Fills
            // do not need their fill-byte input when advancing the cursor.
            src(step == Issue && !move &&
                        (fill || !(commandBytes & (commandBytes - 1)))
                    ? 0
                    : inst.rs1);
            src(finalCount && (step == PublishD || step == PublishS)
                    ? int_reg::_StreamIssueIdx
                    : inst.rs2);
            if (!initialRead && step != PublishD && step != PublishS &&
                (!this->terminal || lengths ||
                 (step != Read && step != Write))) {
                src(int_reg::_StreamIssueIdx);
            }
            if (step == Issue) {
                dst(int_reg::_StreamIssueIdx);
            } else if (step == Read) {
                flags[IsLoad] = true;
                if (scalar) {
                    dst(int_reg::_Ureg0Idx);
                } else {
                    setDestRegIdx(_numDestRegs++,
                                  vecRegClass[VecMemInternalReg0 +
                                              (ordinal % tempRegs)]);
                    _numTypedDestRegs[VecRegClass] = 1;
                    if (deferredPeer && (ordinal & 1)) {
                        setSrcRegIdx(_numSrcRegs++,
                                     vecRegClass[VecMemInternalReg0 +
                                                 ((ordinal - 1) % tempRegs)]);
                    }
                }
            } else if (step == Write) {
                flags[IsStore] = true;
                if (!fill) {
                    if (scalar) {
                        src(int_reg::_Ureg0Idx);
                    } else {
                        setSrcRegIdx(_numSrcRegs++,
                                     vecRegClass[VecMemInternalReg0 +
                                                 (ordinal % tempRegs)]);
                        if (storeStride == 2 && ordinal + 1 < commands) {
                            setSrcRegIdx(
                                _numSrcRegs++,
                                vecRegClass[VecMemInternalReg0 +
                                            ((ordinal + 1) % tempRegs)]);
                        }
                    }
                }
                if (finalCount) {
                    dst(inst.rs2);
                } else if (!lengths) {
                    dst(int_reg::_StreamRemainingIdx);
                }
            } else if (step == BatchEnd) {
                // The no-memory end marker waits for the preceding store's
                // count result before redirecting a partial final batch.
                src(int_reg::_StreamRemainingIdx);
                dst(int_reg::_StreamRemainingIdx);
            } else if (step != Select) {
                dst(step == PublishD ? inst.rd : inst.rs1);
            }
        }
        // Issue retires after all stores in its group, so it is also a
        // restart point. Keeping it delayed can defer a pending interrupt
        // until the end of a long copy when fetch stops just after it.
        flags[IsDelayedCommit] =
            step != Write && step != BatchEnd && step != Issue &&
            step != Continue && step != Select && step != PublishR &&
            step != Zero && !(finalCount && step == PublishD);
        if (step == Zero || step == PublishR ||
            (finalCount && step == PublishD)) {
            flags[IsLastMicroop] = true;
        }
        if (step == InitIssue || step == Issue || step == Continue ||
            step == Select || controlPoint()) {
            flags[IsControl] = true;
            const bool indirect = (lengths && step != Issue) ||
                                  (step == InitIssue && scalarThreshold);
            flags[IsDirectControl] = !indirect;
            flags[IsIndirectControl] = indirect;
            const bool terminal = terminalControl();
            flags[IsCondControl] = !terminal;
            flags[IsUncondControl] = terminal;
            if (lengths && step == InitIssue) {
                // Zero count falls through into a no-op completion. Positive
                // lengths take the ordinary indirect dispatch branch.
                flags[IsCondControl] = true;
                flags[IsUncondControl] = false;
            }
        }
    }

    Fault
    execute(ExecContext *xc, trace::InstRecord *) const override
    {
        if (step == Zero) {
            return NoFault;
        } else if (step == InitIssue) {
            // Copies/fills classify by count alone. Moves retain the address
            // dependencies needed to select their overlapping split schedule.
            const RegVal remaining = value(xc, lengths || move ? 2 : 0);
            PCState pc = xc->pcState().as<PCState>();
            const MicroPC selected =
                lengths
                    ? lengths->select(value(xc, 0), value(xc, 1), remaining,
                                      remaining, beatSize(xc), true)
                    : entryTarget(remaining, move ? value(xc, 0) : 0,
                                  move ? value(xc, 1) : 0);
            RegVal cursor = remaining;
            if (lengths && !move && !(commandBytes & (commandBytes - 1)) &&
                (lengths->isOwnedHead(selected) ||
                 lengths->isFirstWindow(selected))) {
                const unsigned span =
                    lengths->longBytes && (selected == lengths->longEntry ||
                                           selected == lengths->longEntryFinal)
                        ? lengths->longBytes
                        : 0;
                cursor -= lengths->bytes(
                    value(xc, 0), value(xc, 1), remaining, remaining,
                    beatSize(xc),
                    span ? span
                         : lengths->windowCommands(remaining) * beatSize(xc),
                    true, span);
            }
            xc->setRegOperand(this, 0, rvSext(cursor));
            pc.nupc(selected);
            xc->pcState(pc);
        } else if (step == Continue) {
            // A page-limited terminal store publishes its exact progress.
            // Reuse that count as the bulk cursor without repeating entry.
            const RegVal remaining = value(xc, lengths ? 3 : 0);
            xc->setRegOperand(this, 0, rvSext(remaining));
            PCState pc = xc->pcState().as<PCState>();
            pc.nupc(lengths ? lengths->select(value(xc, 0), value(xc, 1),
                                              value(xc, 2), remaining,
                                              beatSize(xc), false)
                            : target);
            xc->pcState(pc);
        } else if (step == Issue) {
            const RegVal next = value(xc, 3) - groupSize(xc);
            xc->setRegOperand(this, 0, rvSext(next));
            const unsigned width = beatSize(xc);
            const unsigned capacity =
                lengths ? std::min(commands > lengths->maxCommands
                                       ? groupBytes
                                       : lengths->maxBytes,
                                   commands * width)
                        : 0;
            const bool full =
                lengths &&
                (commands > lengths->maxCommands ||
                 commands == lengths->windowCommands(value(xc, 2))) &&
                next > capacity &&
                lengths->bytes(value(xc, 0), value(xc, 1), value(xc, 2), next,
                               width, capacity, true,
                               commands > lengths->maxCommands
                                   ? groupBytes
                                   : 0) == capacity;
            if (lengths ? full : next != 0) {
                PCState pc = xc->pcState().as<PCState>();
                pc.nupc(lengths ? lengths->loopTarget(commands, storeStride,
                                                      deferredPeer)
                                : target);
                xc->pcState(pc);
            }
        } else if (step == Select) {
            const RegVal remaining = value(xc, 3);
            // Loop bodies are selected only when a suffix remains. The
            // direct loop likewise requires more than one full group, so
            // the committed path always dispatches a nonempty final suffix.
            PCState pc = xc->pcState().as<PCState>();
            pc.nupc(lengths->select(value(xc, 0), value(xc, 1), value(xc, 2),
                                    remaining, beatSize(xc), false));
            xc->pcState(pc);
        } else if (step == Read) {
            VecRegContainer data;
            data.zero();
            const unsigned size = amount(xc);
            if (size) {
                Fault fault = xc->readMem(
                    address(xc), data.as<uint8_t>(), size,
                    XlateFlags::ZIMEMOPS | XlateFlags::ZIMEMOPS_WIDE,
                    std::vector<bool>(size, true));
                if (fault != NoFault) {
                    return fault;
                }
            }
            loadResult(xc, data.as<uint8_t>(), size);
        } else if (step == Write) {
            return write(xc, true);
        } else if (step == BatchEnd) {
            progress(xc, true);
        } else if (step == PublishR) {
            xc->setRegOperand(this, 0, RegVal(0));
        } else if (!move && (step == PublishD || step == PublishS)) {
            const RegVal done = value(xc, 1);
            xc->setRegOperand(this, 0, rvSext(value(xc, 0) + done));
        } else {
            const RegVal done = value(xc, 2);
            const bool back = reverse(xc);
            const RegVal result =
                value(xc, step == PublishD ? 0 : 1) + (back ? 0 : done);
            xc->setRegOperand(this, 0, rvSext(result));
        }
        return NoFault;
    }

    Fault
    initiateAcc(ExecContext *xc, trace::InstRecord *) const override
    {
        if (step == Write) {
            return write(xc, false);
        }
        assert(step == Read);
        const unsigned size = amount(xc);
        if (!size) {
            xc->setMemAccPredicate(false);
            return NoFault;
        }
        return xc->initiateMemRead(address(xc), size,
                                   XlateFlags::ZIMEMOPS |
                                       XlateFlags::ZIMEMOPS_WIDE,
                                   std::vector<bool>(size, true));
    }

    Fault
    completeAcc(PacketPtr pkt, ExecContext *xc,
                trace::InstRecord *) const override
    {
        if (step == Read) {
            loadResult(xc, pkt ? pkt->getConstPtr<uint8_t>() : nullptr,
                       pkt ? pkt->getSize() : 0);
        } else if (!xc->hasRegisterRenaming() && amount(xc)) {
            progress(xc, controlPoint());
        }
        return NoFault;
    }

    void
    synchronizeState(ThreadContext *tc) const override
    {
        if (step == Zero || step == InitIssue) {
            return;
        }
        const MicroPC pc = tc->pcState().microPC();
        if (pc < 1 || pc >= ZimemopsBytePC) {
            return;
        }
        const auto get = [&](RegIndex reg) {
            return rvZext(tc->getReg(intRegClass[reg]));
        };
        const RegVal d = get(machInst.rd);
        const RegVal s = get(machInst.rs1);
        const RegVal r = get(machInst.rs2);
        if (finalCount && (step == PublishS || step == PublishD ||
                           (step == Write && pc > firstStorePC))) {
            const bool back = move && d > s;
            const RegVal done = back ? 0 : get(int_reg::_StreamIssueIdx);
            if (!fill && pc <= publishPC) {
                tc->setReg(intRegClass[machInst.rs1], rvSext(s + done));
            }
            if (pc <= publishPC + (fill ? 0 : 1)) {
                tc->setReg(intRegClass[machInst.rd], rvSext(d + done));
            }
            // After the single store there are only pointer publications.
            // Rebase their endpoint seed too: (P + N) + 0 == P + N. This
            // preserves in-flight results and permits repeated publication.
            tc->setReg(intRegClass[int_reg::_StreamIssueIdx], RegVal(0));
            return;
        }
        // Before this window's first store, committed progress is its
        // starting cursor. This also covers faults on either initial
        // load, without initializing another copy of the length operand.
        // A dispatch may jump across another body's publication range.
        // Entry has not stored anything yet; a looping Issue has captured
        // the next cursor. Neither has executed publication micro-ops.
        const bool continuing_store =
            terminal && step == Write && pc == target;
        const bool continuing = step == Continue || continuing_store;
        const bool selecting = step == InitIssue || continuing ||
                               step == Select ||
                               (step == Issue && (lengths || pc == target));
        const bool saved_progress =
            continuing_store || (step == Continue && pc != target);
        RegVal left;
        if (endCursor) {
            const RegVal end = get(int_reg::_StreamIssueIdx);
            if (step == Select || step == PublishS || step == PublishD ||
                step == PublishR) {
                left = end;
            } else {
                unsigned stores_done = 0;
                if (pc > firstStorePC) {
                    if (step == Read) {
                        stores_done = ordinal - ordinal % tempRegs;
                    } else if (step == Write) {
                        const unsigned block = ordinal - ordinal % tempRegs;
                        const unsigned beats =
                            std::min(tempRegs, commands - block);
                        const MicroPC store_pc =
                            fill
                                ? firstStorePC + ordinal
                                : firstStorePC - std::min(tempRegs, commands) +
                                      2 * block + beats + ordinal % tempRegs;
                        stores_done = ordinal + (pc != store_pc);
                    }
                }
                if (!stores_done) {
                    left = r;
                } else if (stores_done >= commands) {
                    left = end;
                } else {
                    const unsigned width =
                        std::min(commandBytes,
                                 unsigned(tc->getCpuPtr()->cacheLineSize()));
                    if (!((d + r - end) & (width - 1))) {
                        left = end + (commands - stores_done) * width;
                    } else {
                        const unsigned tail =
                            ((r - end - 1) & (width - 1)) + 1;
                        const unsigned span = (commands - 1) * width + tail;
                        left = end + span - stores_done * width;
                    }
                }
            }
        } else if (!saved_progress && (selecting || pc <= firstStorePC)) {
            left = get(int_reg::_StreamIssueIdx);
        } else if (lengths) {
            if (step == PublishS || step == PublishD || step == PublishR) {
                left = 0;
            } else {
                unsigned stores_done = commands;
                if (step == Read) {
                    stores_done = ordinal - ordinal % tempRegs;
                } else if (step == Write) {
                    const unsigned block = ordinal - ordinal % tempRegs;
                    const unsigned beats =
                        std::min(tempRegs, commands - block);
                    const MicroPC store_pc =
                        fill ? firstStorePC + ordinal / storeStride
                             : firstStorePC - std::min(tempRegs, commands) +
                                   block + block / storeStride + beats +
                                   (ordinal % tempRegs) / storeStride;
                    // A fault is before this store. A stop after retirement
                    // has already advanced past it in the fixed ROM body.
                    stores_done = ordinal + (pc != store_pc
                                                 ? std::min(storeStride,
                                                            commands - ordinal)
                                                 : 0);
                }
                const RegVal issued = get(int_reg::_StreamIssueIdx);
                const unsigned width = std::min(
                    commandBytes, unsigned(tc->getCpuPtr()->cacheLineSize()));
                const unsigned size = lengths->bytes(
                    d, s, r, issued, width, commands * width, !terminal,
                    commands > lengths->maxCommands ? groupBytes : 0);
                const unsigned head = width - ((d + r - issued) & (width - 1));
                const unsigned done = peeledTail && stores_done
                                          ? head + (stores_done - 1) * width
                                          : stores_done * width;
                left = issued - std::min(size, done);
            }
        } else {
            left = get(int_reg::_StreamRemainingIdx);
        }
        const bool back = move && d > s;
        const RegVal done = back ? 0 : r - left;
        const bool source_done = !selecting && !fill && (pc > publishPC);
        const bool dest_done = !selecting && pc > publishPC + (fill ? 0 : 1);
        if (!fill && !source_done) {
            tc->setReg(intRegClass[machInst.rs1], rvSext(s + done));
        }
        if (!dest_done) {
            tc->setReg(intRegClass[machInst.rd], rvSext(d + done));
        }
        // Preserve the window endpoints when publishing a partial prefix:
        // (D + done) + (R - done) == D + R. In-flight commands retain their
        // issue cursor, so rebasing the origin length leaves every address
        // unchanged and permits continuing at the current microPC.
        tc->setReg(intRegClass[machInst.rs2], rvSext(left));
    }

    Addr
    branchPredictionAddr(const PCStateBase &pc) const override
    {
        // Distinct native-command stop tests must not all train one entry
        // at the macro PC. Fold the microPC into the existing predictor key;
        // this consumes the common BPU tables, without adding capacity.
        return pc.instAddr() ^ (Addr(1) << 63) ^ (Addr(pc.microPC()) << 4);
    }

    std::unique_ptr<PCStateBase>
    branchTarget(const PCStateBase &pc) const override
    {
        auto next = std::make_unique<PCState>(pc.as<PCState>());
        next->nupc(
            lengths && step == Issue
                ? lengths->loopTarget(commands, storeStride, deferredPeer)
                : target);
        next->uAdvance();
        return next;
    }

    std::unique_ptr<PCStateBase>
    branchTarget(ThreadContext *tc) const override
    {
        auto next = std::make_unique<PCState>(tc->pcState().as<PCState>());
        if (lengths) {
            const RegVal d = rvZext(tc->getReg(intRegClass[machInst.rd]));
            const RegVal s = rvZext(tc->getReg(intRegClass[machInst.rs1]));
            const RegVal original =
                rvZext(tc->getReg(intRegClass[machInst.rs2]));
            const RegVal remaining =
                step == InitIssue
                    ? original
                    : rvZext(tc->getReg(
                          intRegClass[step == Select
                                          ? int_reg::_StreamIssueIdx
                                          : int_reg::_StreamRemainingIdx]));
            const unsigned width = std::min(
                commandBytes, unsigned(tc->getCpuPtr()->cacheLineSize()));
            next->nupc(
                step == Issue
                    ? lengths->loopTarget(commands, storeStride, deferredPeer)
                    : lengths->select(d, s, original, remaining, width,
                                      step == InitIssue));
            next->uAdvance();
            return next;
        }
        const RegVal remaining = rvZext(tc->getReg(intRegClass[machInst.rs2]));
        const RegVal d =
            move ? rvZext(tc->getReg(intRegClass[machInst.rd])) : 0;
        const RegVal s =
            move ? rvZext(tc->getReg(intRegClass[machInst.rs1])) : 0;
        next->nupc(entryTarget(remaining, d, s));
        next->uAdvance();
        return next;
    }

    std::string
    generateDisassembly(Addr, const loader::SymbolTable *) const override
    {
        static const char *names[] = {
            "init",  "issue",     "continue",  "select",    "load", "store",
            "batch", "publish_d", "publish_s", "publish_r", "zero"};
        return std::string(mnemonic) + (scalar ? ".scalar." : ".vector.") +
               names[step];
    }
};

} // anonymous namespace

Zimemops::Zimemops(const char *mnem, ExtMachInst mach_inst, unsigned unroll,
                   unsigned bytes, unsigned beat_bytes, bool streaming,
                   bool quad_batch, bool pair_control,
                   unsigned scalar_threshold)
    : RiscvMacroInst(mnem, mach_inst, No_OpClass)
{
    assert(unroll >= 1 && unroll <= 64);
    assert(bytes >= 1 && bytes <= 256);
    const unsigned first_ops =
        (mach_inst.funct7 == 6 ? 5 : 7) +
        (bytes > beat_bytes ? (mach_inst.funct7 == 6 ? 3 : 5) : 0);
    const MicroPC body = unroll == 1 ? 0 : first_ops + 1;
    auto append_group = [&](unsigned width, MicroPC target, bool loop) {
        auto add = [&](ZimemopsMicro::Step step, bool second = false) {
            microops.push_back(
                new ZimemopsMicro(mnem, mach_inst, step,
                                  step == ZimemopsMicro::Loop ? target : 0,
                                  width, std::min(width, beat_bytes), second));
        };
        add(ZimemopsMicro::Setup);
        if (mach_inst.funct7 != 6) {
            add(ZimemopsMicro::Read);
            if (width > beat_bytes) {
                add(ZimemopsMicro::Read, true);
            }
        }
        for (unsigned i = 0; i < (width > beat_bytes ? 2 : 1); ++i) {
            add(ZimemopsMicro::Write, i);
            add(ZimemopsMicro::Dest, i);
            if (mach_inst.funct7 != 6) {
                add(ZimemopsMicro::Source, i);
            }
            add(ZimemopsMicro::Count, i);
        }
        if (loop) {
            add(ZimemopsMicro::Loop);
        }
    };
    if (streaming) {
        const bool fill = mach_inst.funct7 == 6;
        const unsigned commands = unroll * (bytes > beat_bytes ? 2 : 1);
        const unsigned command_bytes = std::min(bytes, beat_bytes);
        const unsigned window_bytes = bytes * unroll;
        const unsigned load_width =
            quad_batch && !fill && commands >= 4 ? 4 : 2;
        const unsigned control_width = quad_batch ? 4 : pair_control ? 2 : 1;
        auto append_stream =
            [&](bool scalar, bool terminal, unsigned window, unsigned beat,
                unsigned count, unsigned width, unsigned loads,
                unsigned controls, MicroPC loop, MicroPC resume = 1,
                bool ownership = false,
                std::shared_ptr<const ZimemopsLengths> active_lengths =
                    nullptr) {
                const MicroPC body = microops.size();
                const bool final_count =
                    ownership && active_lengths && terminal && count == 1;
                const MicroPC issue = body + ownership +
                                      (fill ? count : 2 * count) +
                                      (count % controls ? 1 : 0);
                const MicroPC publish = issue + (terminal ? 0 : 1);
                const MicroPC first_store =
                    body + ownership + (fill ? 0 : std::min(loads, count));
                auto add = [&](ZimemopsStream::Step step, unsigned ordinal = 0,
                               MicroPC target = 1) {
                    microops.push_back(new ZimemopsStream(
                        mnem, mach_inst, step, window, beat, count, width,
                        ordinal, target, publish, first_store, controls, loads,
                        scalar, 0, 1, terminal, active_lengths,
                        active_lengths && terminal &&
                            step == ZimemopsStream::Read && ordinal == 0,
                        final_count));
                };
                if (ownership) {
                    microops.push_back(new ZimemopsPrefetch(mnem, mach_inst));
                }
                // Typed temporaries are known at rename. Integer data reuses
                // Ureg0, which is otherwise only live in the reference/replay
                // path. Both classes consume the existing physical reg pools.
                for (unsigned i = 0; i < count; i += loads) {
                    const unsigned beats = std::min(loads, count - i);
                    if (!fill) {
                        for (unsigned j = 0; j < beats; ++j) {
                            add(ZimemopsStream::Read, i + j);
                        }
                    }
                    for (unsigned j = 0; j < beats; ++j) {
                        // A terminal command falls into publication when done.
                        // At a page edge it redirects to the shared
                        // continuation.
                        add(ZimemopsStream::Write, i + j,
                            terminal ? resume : issue);
                    }
                }
                if (count % controls) {
                    add(ZimemopsStream::BatchEnd,
                        count + controls - (count % controls) - 1, issue);
                }
                if (!terminal) {
                    add(ZimemopsStream::Issue, 0, loop);
                }
                // Source-first publication preserves forward-move direction.
                if (!fill) {
                    add(ZimemopsStream::PublishS);
                }
                add(ZimemopsStream::PublishD);
                if (!final_count) {
                    add(ZimemopsStream::PublishR);
                }
            };
        if (!quad_batch && !pair_control) {
            auto lengths = std::make_shared<ZimemopsLengths>();
            lengths->maxBytes = std::min(window_bytes, 4 * command_bytes);
            lengths->tailBytes = std::min(window_bytes, 5 * command_bytes);
            lengths->maxCommands =
                (lengths->maxBytes + command_bytes - 1) / command_bytes;
            lengths->beatBytes = beat_bytes;
            lengths->scalarThreshold = scalar_threshold;
            lengths->wordBytes = mach_inst.rv_type == RV32 ? 4 : 8;
            lengths->fill = fill;
            lengths->move = mach_inst.funct7 == 3;
            const unsigned short_ops = fill ? 3 : 5;
            lengths->zero = 1;
            lengths->scalar = scalar_threshold ? 2 : 0;
            lengths->vector = 2 + (scalar_threshold ? short_ops : 0);
            const MicroPC resume = lengths->vector + short_ops;
            microops.push_back(new ZimemopsStream(
                mnem, mach_inst, ZimemopsStream::InitIssue, lengths->maxBytes,
                beat_bytes, 4, command_bytes, 0, 0, fill ? 2 : 3, fill ? 1 : 2,
                1, 1, false, scalar_threshold, lengths->vector, false,
                lengths));
            microops.push_back(new ZimemopsStream(
                mnem, mach_inst, ZimemopsStream::Zero, lengths->maxBytes,
                beat_bytes, 1, command_bytes));
            if (scalar_threshold) {
                append_stream(
                    true, true, std::min(command_bytes, lengths->wordBytes),
                    beat_bytes, 1, std::min(command_bytes, lengths->wordBytes),
                    1, 1, 0, resume, true, lengths);
            }
            append_stream(false, true, command_bytes, beat_bytes, 1,
                          command_bytes, 1, 1, 0, resume, true, lengths);
            assert(microops.size() == resume);
            microops.push_back(new ZimemopsStream(
                mnem, mach_inst, ZimemopsStream::Continue, lengths->maxBytes,
                beat_bytes, 4, command_bytes, 0, 0, ZimemopsBytePC, 0, 1, 1,
                false, 0, 1, false, lengths));
            const unsigned max_count = lengths->maxCommands;
            auto append_length = [&](unsigned count, bool terminal,
                                     bool initial = false,
                                     bool ownership = false,
                                     bool end_ownership = false,
                                     unsigned load_batch = 0,
                                     bool peeled_tail = false,
                                     unsigned store_stride = 1,
                                     bool end_cursor = false,
                                     bool first_aligned = false,
                                     bool deferred_peer = false) {
                const MicroPC body = microops.size();
                const unsigned span = count > max_count ? count * command_bytes
                                                        : lengths->maxBytes;
                const unsigned prefetches = ownership + end_ownership;
                const unsigned loads = load_batch   ? load_batch
                                       : initial    ? 1
                                       : count == 3 ? 2
                                                    : count;
                const MicroPC first_store =
                    body + prefetches + (fill ? 0 : loads);
                const MicroPC publish =
                    terminal ? body + prefetches + (fill ? 0 : count) +
                                   (count + store_stride - 1) / store_stride
                             : ZimemopsBytePC;
                auto add = [&](ZimemopsStream::Step step,
                               unsigned ordinal = 0) {
                    microops.push_back(new ZimemopsStream(
                        mnem, mach_inst, step, span, beat_bytes, count,
                        command_bytes, ordinal, 0, publish, first_store, 1,
                        loads, false, 0, 1, terminal, lengths,
                        initial && ordinal == 0 &&
                            (step == ZimemopsStream::Read ||
                             (first_aligned && step == ZimemopsStream::Write)),
                        false, peeled_tail, store_stride, end_cursor,
                        first_aligned, deferred_peer));
                };
                if (fill && (ownership || end_ownership)) {
                    if (ownership) {
                        microops.push_back(
                            new ZimemopsPrefetch(mnem, mach_inst));
                    }
                    if (end_ownership) {
                        microops.push_back(
                            new ZimemopsPrefetch(mnem, mach_inst, true));
                    }
                }
                // Only active beats exist. Bounded load batches let older
                // stores enter the shared LSU before the next batch's loads.
                for (unsigned i = 0; i < count; i += loads) {
                    const unsigned beats = std::min(loads, count - i);
                    if (!fill) {
                        for (unsigned j = 0; j < beats; ++j) {
                            const unsigned index = deferred_peer && beats == 4
                                                       ? (j & 1) * 2 + j / 2
                                                       : j;
                            add(ZimemopsStream::Read, i + index);
                            if ((ownership || end_ownership) && i + j == 0) {
                                if (ownership) {
                                    microops.push_back(
                                        new ZimemopsPrefetch(mnem, mach_inst));
                                }
                                if (end_ownership) {
                                    microops.push_back(new ZimemopsPrefetch(
                                        mnem, mach_inst, true));
                                }
                            }
                        }
                    }
                    for (unsigned j = 0; j < beats; j += store_stride) {
                        add(ZimemopsStream::Write, i + j);
                    }
                }
                if (!terminal) {
                    if (!end_cursor) {
                        add(ZimemopsStream::Issue);
                    }
                    add(ZimemopsStream::Select);
                }
                if (terminal) {
                    if (!fill) {
                        add(ZimemopsStream::PublishS);
                    }
                    add(ZimemopsStream::PublishD);
                    add(ZimemopsStream::PublishR);
                }
            };
            if (max_count >= 2) {
                lengths->doubleHead = microops.size();
                if (lengths->move) {
                    append_length(2, true, true);
                } else {
                    append_length(2, true, true, false, false, 2, false, 1,
                                  !(command_bytes & (command_bytes - 1)),
                                  true);
                }
                lengths->ownedDoubleHead = microops.size();
                append_length(2, true, true, true);
                lengths->ownedDoubleEdges = microops.size();
                append_length(2, true, true, true, true);
                if (!fill && !lengths->move) {
                    lengths->deferredDoubleHead = microops.size();
                    append_length(2, true, true, false, false, 2, false, 1,
                                  true, true, true);
                }
            }
            if (lengths->tailBytes > lengths->maxBytes) {
                lengths->fiveCommandTail = microops.size();
                append_length(5, true, false, false, false, 2);
                if (!lengths->move) {
                    lengths->pairedFiveTail = microops.size();
                    append_length(5, true, true, false, false, 2, false, 2);
                    if (!fill) {
                        lengths->deferredFiveTail = microops.size();
                        append_length(5, true, true, false, true, 2, false, 2,
                                      false, false, true);
                    }
                }
            }
            for (unsigned count = 3; count <= max_count + 1; ++count) {
                lengths->ownedTails[count] = microops.size();
                append_length(count, true, true, true, true,
                              std::min(count, 2U), true);
                lengths->ownedLineTails[count] = microops.size();
                append_length(count, true, true, true, true,
                              std::min(count, 3U), true);
            }
            if (!lengths->move) {
                if (!fill) {
                    lengths->longBytes =
                        std::min(window_bytes, 16 * command_bytes);
                    if (lengths->longBytes > lengths->tailBytes &&
                        lengths->longBytes % command_bytes == 0) {
                        const unsigned count =
                            lengths->longBytes / command_bytes;
                        lengths->longLoop = microops.size();
                        append_length(count, false, false, false, false, 4,
                                      false, 1, false, false, true);
                        lengths->longFinal = microops.size();
                        append_length(count, true, false, false, false, 4,
                                      false, 1, false, false, true);
                        lengths->longEntry = microops.size();
                        append_length(count, false, true, false, false, 4,
                                      false, 1, true, true, true);
                        lengths->longEntryFinal = microops.size();
                        append_length(count, true, true, false, false, 4,
                                      false, 1, true, true, true);
                    } else {
                        lengths->longBytes = 0;
                    }
                    for (unsigned count = 2; count <= max_count; ++count) {
                        const unsigned loads = count == 3 ? 2 : count;
                        lengths->deferredLoops[count] = microops.size();
                        append_length(count, false, false, false, false, loads,
                                      false, 1, false, false, true);
                        lengths->deferredFinals[count] = microops.size();
                        append_length(count, true, false, false, count == 3,
                                      loads, false, 1, false, false, true);
                        lengths->deferredEntries[count] = microops.size();
                        append_length(count, false, true, false, false, loads,
                                      false, 1, false, false, true);
                        lengths->deferredEntryFinals[count] = microops.size();
                        append_length(count, true, true, false, count == 3,
                                      loads, false, 1, false, false, true);
                    }
                }
                for (unsigned count = 2; count <= max_count; ++count) {
                    const unsigned loads = count == 3 ? 2 : count;
                    lengths->pairedLoops[count] = microops.size();
                    append_length(count, false, false, false, false, loads,
                                  false, 2);
                    lengths->pairedFinals[count] = microops.size();
                    append_length(count, true, false, false, false, loads,
                                  false, 2);
                    lengths->pairedEntries[count] = microops.size();
                    append_length(
                        count, false, true, false, false, loads, false, 1,
                        !(command_bytes & (command_bytes - 1)), true);
                    lengths->pairedEntryFinals[count] = microops.size();
                    append_length(
                        count, true, true, false, false, loads, false, 1,
                        !(command_bytes & (command_bytes - 1)), true);
                }
            }
            for (unsigned count = 1; count <= max_count; ++count) {
                lengths->ownedLoops[count] = microops.size();
                append_length(count, false, true, true, false,
                              count == 3 ? 2 : count, false, 1,
                              !lengths->move &&
                                  !(command_bytes & (command_bytes - 1)));
                lengths->bareHeads[count] = microops.size();
                append_length(count, false, true, fill, false,
                              count == 3 ? 2 : count, false, 1,
                              !lengths->move &&
                                  !(command_bytes & (command_bytes - 1)));
            }
            for (unsigned count = 1; count <= max_count; ++count) {
                lengths->loops[count] = microops.size();
                append_length(count, false);
            }
            for (unsigned count = 1; count <= max_count; ++count) {
                lengths->finals[count] = microops.size();
                append_length(count, true);
            }
        } else {
            const unsigned short_ops = fill ? 3 : 5;
            const MicroPC short_start = 1 + (scalar_threshold ? short_ops : 0);
            const MicroPC resume = short_start + short_ops;
            const MicroPC bulk = resume + 1;
            // The entry result captures only the count. With both data classes
            // enabled, the body choice is an ordinary indirect branch using
            // the common BPU; no extra selector state or execution resource
            // exists.
            microops.push_back(new ZimemopsStream(
                mnem, mach_inst, ZimemopsStream::InitIssue, window_bytes,
                beat_bytes, commands, command_bytes, 0, bulk, fill ? 2 : 3,
                fill ? 1 : 2, control_width, load_width, false,
                scalar_threshold, short_start));
            if (scalar_threshold) {
                const unsigned word = mach_inst.rv_type == RV32 ? 4 : 8;
                append_stream(true, true, std::min(command_bytes, word),
                              beat_bytes, 1, std::min(command_bytes, word), 1,
                              1, bulk, resume);
            }
            assert(microops.size() == short_start);
            append_stream(false, true, command_bytes, beat_bytes, 1,
                          command_bytes, 1, 1, bulk, resume);
            assert(microops.size() == resume);
            microops.push_back(new ZimemopsStream(
                mnem, mach_inst, ZimemopsStream::Continue, command_bytes,
                beat_bytes, 1, command_bytes, 0, bulk));
            assert(microops.size() == bulk);
            append_stream(false, false, window_bytes, beat_bytes, commands,
                          command_bytes, load_width, control_width, bulk);
        }
    } else {
        append_group(bytes, body, true);
        microops.push_back(
            new ZimemopsMicro(mnem, mach_inst, ZimemopsMicro::End));
        if (unroll > 1) {
            for (unsigned i = 0; i < unroll; ++i) {
                append_group(bytes, body, i + 1 == unroll);
            }
            microops.push_back(
                new ZimemopsMicro(mnem, mach_inst, ZimemopsMicro::End));
        }
    }
    microops.front()->setFlag(IsFirstMicroop);
    byteOffset = microops.size();
    assert(byteOffset < ZimemopsBytePC);
    append_group(1, ZimemopsBytePC, true);
    microops.push_back(new ZimemopsMicro(mnem, mach_inst, ZimemopsMicro::End));
}

StaticInstPtr
Zimemops::fetchMicroop(MicroPC upc) const
{
    if (upc >= ZimemopsBytePC) {
        upc = byteOffset + upc - ZimemopsBytePC;
    }
    assert(upc < microops.size());
    return microops[upc];
}

bool
Zimemops::validRegisters(ExtMachInst inst)
{
    return inst.rd && inst.rs2 && inst.rd != inst.rs1 && inst.rd != inst.rs2 &&
           inst.rs1 != inst.rs2 && (inst.funct7 == 6 || inst.rs1);
}

std::string
Zimemops::generateDisassembly(Addr pc, const loader::SymbolTable *symtab) const
{
    std::stringstream ss;
    ss << mnemonic << " (" << registerName(intRegClass[machInst.rd]) << "), ";
    if (machInst.funct7 != 6) {
        ss << '(';
    }
    ss << registerName(intRegClass[machInst.rs1]);
    if (machInst.funct7 != 6) {
        ss << ')';
    }
    ss << ", " << registerName(intRegClass[machInst.rs2]);
    return ss.str();
}

} // namespace RiscvISA
} // namespace gem5
