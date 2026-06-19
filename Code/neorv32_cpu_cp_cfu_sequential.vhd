library ieee;
use ieee.std_logic_1164.all;
use ieee.numeric_std.all;

entity neorv32_cpu_cp_cfu is
  port (
    clk_i    : in  std_ulogic;
    rstn_i   : in  std_ulogic;
    start_i  : in  std_ulogic;
    active_i : in  std_ulogic;
    rtype_i  : in  std_ulogic;
    funct3_i : in  std_ulogic_vector(2 downto 0);
    funct7_i : in  std_ulogic_vector(6 downto 0);
    rs1_i    : in  std_ulogic_vector(31 downto 0);
    rs2_i    : in  std_ulogic_vector(31 downto 0);
    rs3_i    : in  std_ulogic_vector(31 downto 0);
    result_o : out std_ulogic_vector(31 downto 0);
    valid_o  : out std_ulogic
  );
end neorv32_cpu_cp_cfu;

architecture neorv32_cpu_cp_cfu_rtl of neorv32_cpu_cp_cfu is

  constant r4type_c : std_ulogic := '1';

  constant THRESHOLD : integer := 50;

  signal clause_failed      : std_ulogic;               
  signal clause_all_exclude : std_ulogic;               
  signal class_sum          : signed(7 downto 0);       

  signal valid_reg  : std_ulogic;
  signal result_reg : std_ulogic_vector(31 downto 0);

begin

  result_o <= result_reg;
  valid_o  <= valid_reg;

  cfu_seq: process(clk_i, rstn_i)
    variable x_bits      : std_ulogic_vector(31 downto 0);
    variable pos_mask    : std_ulogic_vector(31 downto 0);
    variable neg_mask    : std_ulogic_vector(31 downto 0);
    variable mismatch    : std_ulogic;
    variable all_zero    : std_ulogic;
    variable vote        : signed(7 downto 0);
    variable new_sum     : signed(7 downto 0);
    constant zero_vec    : std_ulogic_vector(31 downto 0) := (others => '0');
  begin
    if rstn_i = '0' then
      clause_failed      <= '0';
      clause_all_exclude <= '1';
      class_sum          <= (others => '0');
      valid_reg          <= '0';
      result_reg         <= (others => '0');

    elsif rising_edge(clk_i) then
      valid_reg  <= '0';
      result_reg <= (others => '0');

      if (start_i = '1') and (rtype_i = r4type_c) then
        valid_reg <= '1';  -- all operations complete in one cycle

        case funct3_i is

          -- ----------------------------------------------------------------
          -- funct3 = 000: CHUNK_EVAL
          -- Update clause_failed and clause_all_exclude based on this chunk.
          -- ----------------------------------------------------------------
          when "000" =>
            x_bits   := rs1_i;
            pos_mask := rs2_i;
            neg_mask := rs3_i;

            if ((pos_mask and (not x_bits)) /= zero_vec) or
               ((neg_mask and x_bits) /= zero_vec) then
              mismatch := '1';
            else
              mismatch := '0';
            end if;

            if (pos_mask or neg_mask) = zero_vec then
              all_zero := '1';
            else
              all_zero := '0';
            end if;

            if mismatch = '1' then
              clause_failed <= '1';
            end if;

            if all_zero = '0' then
              clause_all_exclude <= '0';
            end if;

            if (mismatch = '1') or (clause_failed = '1') then
              result_reg(0) <= '1';
            else
              result_reg(0) <= '0';
            end if;
            result_reg(31 downto 1) <= (others => '0');

          -- ----------------------------------------------------------------
          -- funct3 = 001: CLAUSE_COMMIT
          -- Finalize clause vote and add to class_sum.
          -- rs1(0) = polarity: 0 = positive vote (+1), 1 = negative vote (-1)
          -- ----------------------------------------------------------------
          when "001" =>
            if (clause_failed = '0') and (clause_all_exclude = '0') then
              if rs1_i(0) = '0' then
                vote := to_signed(1, 8);   -- even clause: +1
              else
                vote := to_signed(-1, 8);  -- odd clause:  -1
              end if;

              new_sum := class_sum + vote;

              if new_sum > to_signed(THRESHOLD, 8) then
                class_sum <= to_signed(THRESHOLD, 8);
              elsif new_sum < to_signed(-THRESHOLD, 8) then
                class_sum <= to_signed(-THRESHOLD, 8);
              else
                class_sum <= new_sum;
              end if;
            end if;

            clause_failed      <= '0';
            clause_all_exclude <= '1';

            result_reg <= (others => '0');

          -- ----------------------------------------------------------------
          -- funct3 = 010: GET_SCORE
          -- Return clamped class_sum and reset for next class.
          -- ----------------------------------------------------------------
          when "010" =>
            
            result_reg <= std_ulogic_vector(resize(class_sum, 32));

            -- reset for next class
            class_sum          <= (others => '0');
            clause_failed      <= '0';
            clause_all_exclude <= '1';

          when others =>
            null;

        end case;
      end if;
    end if;
  end process cfu_seq;

end neorv32_cpu_cp_cfu_rtl;