# Copyright 2021 Google, Inc.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from m5.objects.InstDecoder import InstDecoder
from m5.params import *


class RiscvDecoder(InstDecoder):
    type = "RiscvDecoder"
    cxx_class = "gem5::RiscvISA::Decoder"
    cxx_header = "arch/riscv/decoder.hh"

    zimemops_unroll = Param.Unsigned(
        16, "Transfers per Zimemops microcode loop (1 to 64)"
    )
    zimemops_bytes = Param.Unsigned(
        64,
        "Copy descriptor bytes, capped at two native 32-byte beats (1 to 256)",
    )
    zimemops_streaming = Param.Bool(
        True, "Keep streaming progress private until completion or a stop"
    )
    zimemops_quad_batch = Param.Bool(
        False, "Use four native beats per Zimemops load/store microcode batch"
    )
    zimemops_pair_control = Param.Bool(
        False, "Redirect Zimemops microcode after every pair of native stores"
    )
    zimemops_scalar_threshold = Param.Unsigned(
        0,
        "Use integer data for fragments up to this size (0 disables, max 8)",
    )
