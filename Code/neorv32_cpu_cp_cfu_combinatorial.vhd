-- ================================================================================ --
-- NEORV32 CPU - Co-Processor: Custom (RISC-V Instructions) Functions Unit (CFU)    --
-- -------------------------------------------------------------------------------- --
-- For custom/user-defined RISC-V instructions. See the CPU's documentation for     --
-- more information. Also take a look at the "software-counterpart" of this default --
-- CFU hardware example in 'sw/example/demo_cfu'.                                   --
-- -------------------------------------------------------------------------------- --
-- The NEORV32 RISC-V Processor - https://github.com/stnolting/neorv32              --
-- Copyright (c) NEORV32 contributors.                                              --
-- Copyright (c) 2020 - 2025 Stephan Nolting. All rights reserved.                  --
-- Licensed under the BSD-3-Clause license, see LICENSE for details.                --
-- SPDX-License-Identifier: BSD-3-Clause                                            --
-- ================================================================================ --

library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity neorv32_cpu_cp_cfu is
  port (
    -- global control --
    clk_i    : in  std_ulogic; -- global clock, rising edge
    rstn_i   : in  std_ulogic; -- global reset, low-active, async
    -- operation control --
    start_i  : in  std_ulogic; -- operation trigger/strobe
    active_i : in  std_ulogic; -- operation in progress, CPU is waiting for CFU
    -- operands (from/via custom instruction word) --
    rtype_i  : in  std_ulogic; -- instruction type (0 = R3-type, 1 = R4-type); from instruction word's "opcode[5]" bit
    funct3_i : in  std_ulogic_vector(2 downto 0); -- "funct3" bit-field from custom instruction word
    funct7_i : in  std_ulogic_vector(6 downto 0); -- "funct7" bit-field from custom instruction word
    rs1_i    : in  std_ulogic_vector(31 downto 0); -- rf source 1 via "rs1" bit-field from custom instruction word
    rs2_i    : in  std_ulogic_vector(31 downto 0); -- rf source 2 via "rs2" bit-field from custom instruction word
    rs3_i    : in  std_ulogic_vector(31 downto 0); -- rf source 3 via "rs3" bit-field from custom instruction word
    -- result and status --
    result_o : out std_ulogic_vector(31 downto 0); -- operation result
    valid_o  : out std_ulogic -- result valid, operation done; set one cycle before result_o is valid
  );
end neorv32_cpu_cp_cfu;

architecture neorv32_cpu_cp_cfu_rtl of neorv32_cpu_cp_cfu is

  -- CFU instruction type formats --
  constant r3type_c : std_ulogic := '0'; -- R3-type CFU instructions (custom-0 opcode)
  constant r4type_c : std_ulogic := '1'; -- R4-type CFU instructions (custom-1 opcode)

begin

  result_select: process(rtype_i, funct3_i, rs1_i, rs2_i, rs3_i, start_i, active_i)
    variable x_bits       : std_ulogic_vector(31 downto 0);
    variable pos_mask     : std_ulogic_vector(31 downto 0);
    variable neg_mask     : std_ulogic_vector(31 downto 0);
    variable mismatch_pos : std_ulogic;
    variable mismatch_neg : std_ulogic;
    variable all_zero     : std_ulogic;
    variable clause_out   : std_ulogic;
    constant zero_vec     : std_ulogic_vector(31 downto 0) := (others => '0');
  begin
    -- default outputs
    result_o <= (others => '0');
    valid_o  <= '0';

    if (rtype_i = r4type_c) then
        case funct3_i is
            when "000" => -- 32 bit clause eval
                x_bits   := rs1_i(31 downto 0);
                pos_mask := rs2_i(31 downto 0);
                neg_mask := rs3_i(31 downto 0);
                
                if (pos_mask and (not x_bits)) /= zero_vec then
                    mismatch_pos := '1';
                else
                    mismatch_pos := '0';
                end if;
          
                if (neg_mask and x_bits) /= zero_vec then
                    mismatch_neg := '1';
                else
                    mismatch_neg := '0';
                end if;
          
                if (pos_mask or neg_mask) = zero_vec then
                    all_zero := '1';
                else
                    all_zero := '0';
                end if;

                -- clause_out = 1 if no mismatch and not all_zero
                if (mismatch_pos = '0' and mismatch_neg = '0' and all_zero = '0') then
                    clause_out := '1';
                else
                    clause_out := '0';
                end if;

                result_o(0) <= clause_out;
                result_o(1) <= all_zero;
                valid_o     <= '1';  -- combinational, result ready immediately

        when others =>
          -- unsupported funct3: leave defaults (result=0, valid=0 -> illegal instr)
          null;
        end case;
    end if;
end process result_select;

end neorv32_cpu_cp_cfu_rtl;
