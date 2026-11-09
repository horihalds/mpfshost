#include "core/disasm/x86_decoder.hpp"

#include <Zydis/Decoder.h>
#include <Zydis/Formatter.h>

#include <array>
#include <cstdio>

namespace mpfshost::disasm
{
    namespace
    {
        constexpr std::size_t kMaxInstructionLength = 15;

        struct Prefix
        {
            bool operand16 {};
            bool address32 {};
            bool rex {};
            bool rexW {};
            bool rexR {};
            bool rexX {};
            bool rexB {};
        };

        struct Operand
        {
            bool         memory {};
            bool         ripRelative {};
            int          base {-1};
            int          index {-1};
            int          scale {1};
            std::int64_t displacement {};
        };

        struct Candidate
        {
            bool          store {};
            std::size_t   end {};
            std::uint32_t width {};
            std::string   text;
        };

        bool readByte(std::span<const std::byte> bytes, std::size_t& position, std::uint8_t& value)
        {
            if (position >= bytes.size())
            {
                return false;
            }
            value = static_cast<std::uint8_t>(bytes[position]);
            ++position;
            return true;
        }

        bool
        readInteger(std::span<const std::byte> bytes, std::size_t& position, std::size_t size, std::uint64_t& value)
        {
            if (size == 0 || size > sizeof(value) || position + size > bytes.size())
            {
                return false;
            }
            value = 0;
            for (std::size_t index = 0; index < size; ++index)
            {
                value |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(bytes[position + index])) << (8 * index);
            }
            position += size;
            return true;
        }

        bool readSigned(std::span<const std::byte> bytes, std::size_t& position, std::size_t size, std::int64_t& value)
        {
            std::uint64_t raw = 0;
            if (!readInteger(bytes, position, size, raw))
            {
                return false;
            }
            if (size < sizeof(raw))
            {
                const auto signBit = std::uint64_t {1} << (8 * size - 1);
                if ((raw & signBit) != 0)
                {
                    raw |= ~((std::uint64_t {1} << (8 * size)) - 1);
                }
            }
            value = static_cast<std::int64_t>(raw);
            return true;
        }

        bool parsePrefixes(std::span<const std::byte> bytes, std::size_t& position, Prefix& prefix)
        {
            while (position < bytes.size())
            {
                const auto value = static_cast<std::uint8_t>(bytes[position]);
                switch (value)
                {
                case 0x66:
                    prefix.operand16 = true;
                    ++position;
                    continue;
                case 0x67:
                    prefix.address32 = true;
                    ++position;
                    continue;
                case 0xF0:
                case 0xF2:
                case 0xF3:
                case 0x26:
                case 0x2E:
                case 0x36:
                case 0x3E:
                case 0x64:
                case 0x65:
                    ++position;
                    continue;
                default:
                    break;
                }

                if ((value & 0xF0) == 0x40)
                {
                    prefix.rex  = true;
                    prefix.rexW = (value & 0x08) != 0;
                    prefix.rexR = (value & 0x04) != 0;
                    prefix.rexX = (value & 0x02) != 0;
                    prefix.rexB = (value & 0x01) != 0;
                    ++position;
                    continue;
                }
                break;
            }
            return position < bytes.size();
        }

        std::uint32_t operandWidth(const Prefix& prefix) noexcept
        {
            if (prefix.operand16)
            {
                return 2;
            }
            return prefix.rexW ? 8 : 4;
        }

        std::string_view sizeName(std::uint32_t width) noexcept
        {
            switch (width)
            {
            case 1:
                return "byte";
            case 2:
                return "word";
            case 4:
                return "dword";
            case 8:
                return "qword";
            default:
                return "byte";
            }
        }

        const char* registerName(int index, std::uint32_t width, bool rex) noexcept
        {
            static constexpr std::array<const char*, 16> kByteNoRex = {"al",
                                                                       "cl",
                                                                       "dl",
                                                                       "bl",
                                                                       "ah",
                                                                       "ch",
                                                                       "dh",
                                                                       "bh",
                                                                       "r8b",
                                                                       "r9b",
                                                                       "r10b",
                                                                       "r11b",
                                                                       "r12b",
                                                                       "r13b",
                                                                       "r14b",
                                                                       "r15b"};
            static constexpr std::array<const char*, 16> kByte      = {"al",
                                                                       "cl",
                                                                       "dl",
                                                                       "bl",
                                                                       "spl",
                                                                       "bpl",
                                                                       "sil",
                                                                       "dil",
                                                                       "r8b",
                                                                       "r9b",
                                                                       "r10b",
                                                                       "r11b",
                                                                       "r12b",
                                                                       "r13b",
                                                                       "r14b",
                                                                       "r15b"};
            static constexpr std::array<const char*, 16> kWord      = {"ax",
                                                                       "cx",
                                                                       "dx",
                                                                       "bx",
                                                                       "sp",
                                                                       "bp",
                                                                       "si",
                                                                       "di",
                                                                       "r8w",
                                                                       "r9w",
                                                                       "r10w",
                                                                       "r11w",
                                                                       "r12w",
                                                                       "r13w",
                                                                       "r14w",
                                                                       "r15w"};
            static constexpr std::array<const char*, 16> kDword     = {"eax",
                                                                       "ecx",
                                                                       "edx",
                                                                       "ebx",
                                                                       "esp",
                                                                       "ebp",
                                                                       "esi",
                                                                       "edi",
                                                                       "r8d",
                                                                       "r9d",
                                                                       "r10d",
                                                                       "r11d",
                                                                       "r12d",
                                                                       "r13d",
                                                                       "r14d",
                                                                       "r15d"};
            static constexpr std::array<const char*, 16> kQword     = {"rax",
                                                                       "rcx",
                                                                       "rdx",
                                                                       "rbx",
                                                                       "rsp",
                                                                       "rbp",
                                                                       "rsi",
                                                                       "rdi",
                                                                       "r8",
                                                                       "r9",
                                                                       "r10",
                                                                       "r11",
                                                                       "r12",
                                                                       "r13",
                                                                       "r14",
                                                                       "r15"};

            if (index < 0 || index >= 16)
            {
                return "?";
            }
            switch (width)
            {
            case 1:
                return rex ? kByte[static_cast<std::size_t>(index)] : kByteNoRex[static_cast<std::size_t>(index)];
            case 2:
                return kWord[static_cast<std::size_t>(index)];
            case 8:
                return kQword[static_cast<std::size_t>(index)];
            default:
                return kDword[static_cast<std::size_t>(index)];
            }
        }

        std::string formatDisplacement(std::int64_t displacement)
        {
            char buffer[32] {};
            if (displacement < 0)
            {
                std::snprintf(
                    buffer, sizeof(buffer), "-0x%llx", static_cast<unsigned long long>(-(displacement + 1)) + 1);
            }
            else
            {
                std::snprintf(buffer, sizeof(buffer), "+0x%llx", static_cast<unsigned long long>(displacement));
            }
            return buffer;
        }

        std::string formatOperand(const Operand& operand, std::uint32_t addressWidth, bool rex)
        {
            std::string text  = "[";
            bool        first = true;
            if (operand.ripRelative)
            {
                text += "rip";
                first = false;
            }
            if (operand.base >= 0)
            {
                text += registerName(operand.base, addressWidth, rex);
                first = false;
            }
            if (operand.index >= 0)
            {
                if (!first)
                {
                    text += " + ";
                }
                text += registerName(operand.index, addressWidth, rex);
                if (operand.scale != 1)
                {
                    text += "*";
                    text += std::to_string(operand.scale);
                }
                first = false;
            }
            if (operand.displacement != 0 || first)
            {
                if (!first && operand.displacement > 0)
                {
                    text += " + ";
                    text += "0x";
                    char buffer[24] {};
                    std::snprintf(
                        buffer, sizeof(buffer), "%llx", static_cast<unsigned long long>(operand.displacement));
                    text += buffer;
                }
                else if (first)
                {
                    char buffer[24] {};
                    std::snprintf(
                        buffer, sizeof(buffer), "%llx", static_cast<unsigned long long>(operand.displacement));
                    text += buffer;
                }
                else
                {
                    text += formatDisplacement(operand.displacement);
                }
            }
            text += "]";
            return text;
        }

        bool parseModRm(std::span<const std::byte> bytes,
                        std::size_t&               position,
                        const Prefix&              prefix,
                        Operand&                   operand,
                        int&                       registerIndex)
        {
            std::uint8_t modrm = 0;
            if (!readByte(bytes, position, modrm))
            {
                return false;
            }

            const int mod = (modrm >> 6) & 0x3;
            const int reg = ((modrm >> 3) & 0x7) | (prefix.rexR ? 8 : 0);
            const int rm  = (modrm & 0x7) | (prefix.rexB ? 8 : 0);
            registerIndex = reg;

            if (mod == 3)
            {
                operand.memory = false;
                operand.base   = rm;
                return true;
            }

            operand.memory = true;
            if ((modrm & 0x7) == 4)
            {
                std::uint8_t sib = 0;
                if (!readByte(bytes, position, sib))
                {
                    return false;
                }
                const int scale = 1 << (sib >> 6);
                const int index = ((sib >> 3) & 0x7) | (prefix.rexX ? 8 : 0);
                const int base  = (sib & 0x7) | (prefix.rexB ? 8 : 0);
                if (index != 4 || prefix.rexX)
                {
                    operand.index = index;
                    operand.scale = scale;
                }
                if (mod == 0 && (sib & 0x7) == 5)
                {
                    if (!readSigned(bytes, position, 4, operand.displacement))
                    {
                        return false;
                    }
                }
                else
                {
                    operand.base = base;
                }
            }
            else if (mod == 0 && (modrm & 0x7) == 5)
            {
                operand.ripRelative = !prefix.address32;
                if (!readSigned(bytes, position, 4, operand.displacement))
                {
                    return false;
                }
            }
            else
            {
                operand.base = rm;
            }

            if (mod == 1)
            {
                return readSigned(bytes, position, 1, operand.displacement);
            }
            if (mod == 2)
            {
                return readSigned(bytes, position, 4, operand.displacement);
            }
            return true;
        }

        std::string registerOperand(int index, std::uint32_t width, bool rex)
        {
            return registerName(index, width, rex);
        }

        std::string
        storeText(const char* mnemonic, std::uint32_t width, const std::string& memory, const std::string& source)
        {
            std::string text = mnemonic;
            text += ' ';
            text += sizeName(width);
            text += ' ';
            text += memory;
            if (!source.empty())
            {
                text += ", ";
                text += source;
            }
            return text;
        }

        std::optional<Candidate> decodeOne(std::span<const std::byte> bytes, std::size_t start)
        {
            std::size_t position = start;
            Prefix      prefix;
            if (!parsePrefixes(bytes, position, prefix))
            {
                return std::nullopt;
            }

            std::uint8_t opcode = 0;
            if (!readByte(bytes, position, opcode))
            {
                return std::nullopt;
            }

            const std::uint32_t width = operandWidth(prefix);
            Candidate           candidate;

            if (opcode == 0x0F)
            {
                std::uint8_t second = 0;
                if (!readByte(bytes, position, second) || second < 0x90 || second > 0x9F)
                {
                    return std::nullopt;
                }
                static constexpr std::array<const char*, 16> kSetcc = {"seto",
                                                                       "setno",
                                                                       "setb",
                                                                       "setae",
                                                                       "sete",
                                                                       "setne",
                                                                       "setbe",
                                                                       "seta",
                                                                       "sets",
                                                                       "setns",
                                                                       "setp",
                                                                       "setnp",
                                                                       "setl",
                                                                       "setge",
                                                                       "setle",
                                                                       "setg"};
                Operand                                      operand;
                int                                          registerIndex = 0;
                if (!parseModRm(bytes, position, prefix, operand, registerIndex) || !operand.memory)
                {
                    return std::nullopt;
                }
                candidate.store = true;
                candidate.width = 1;
                candidate.text  = storeText(kSetcc[static_cast<std::size_t>(second) - 0x90],
                                            1,
                                            formatOperand(operand, prefix.address32 ? 4 : 8, prefix.rex),
                                            {});
                candidate.end   = position;
                return candidate;
            }

            if (opcode == 0xAA || opcode == 0xAB)
            {
                const auto stringWidth = opcode == 0xAA ? std::uint32_t {1} : width;
                candidate.store        = true;
                candidate.width        = stringWidth;
                candidate.text         = "stos ";
                candidate.text += sizeName(stringWidth);
                candidate.text += " [rdi]";
                candidate.end = position;
                return candidate;
            }

            const char* mnemonic               = nullptr;
            bool        immediate              = false;
            std::size_t immediateSize          = 0;
            bool        requireGroupOperation  = false;
            int         expectedGroupOperation = 0;

            if (opcode == 0x88 || opcode == 0x89)
            {
                mnemonic = "mov";
            }
            else if (opcode == 0x86 || opcode == 0x87)
            {
                mnemonic = "xchg";
            }
            else if (opcode == 0xC6 || opcode == 0xC7)
            {
                mnemonic               = "mov";
                immediate              = true;
                immediateSize          = opcode == 0xC6 ? 1 : (prefix.operand16 ? 2 : 4);
                requireGroupOperation  = true;
                expectedGroupOperation = 0;
            }
            else if (opcode == 0x80 || opcode == 0x81 || opcode == 0x83)
            {
                static constexpr std::array<const char*, 7> kArithmetic = {
                    "add", "or", "adc", "sbb", "and", "sub", "xor"};
                std::size_t  probe = position;
                std::uint8_t modrm = 0;
                if (!readByte(bytes, probe, modrm))
                {
                    return std::nullopt;
                }
                const int operation = (modrm >> 3) & 0x7;
                if (operation == 7) // cmp does not write memory
                {
                    return std::nullopt;
                }
                immediate     = true;
                immediateSize = opcode == 0x81 ? (prefix.operand16 ? 2 : 4) : 1;
                mnemonic      = kArithmetic[static_cast<std::size_t>(operation)];
            }
            else if (opcode == 0xF6 || opcode == 0xF7 || opcode == 0xFE || opcode == 0xFF)
            {
                std::size_t  probe = position;
                std::uint8_t modrm = 0;
                if (!readByte(bytes, probe, modrm))
                {
                    return std::nullopt;
                }
                const int operation = (modrm >> 3) & 0x7;
                if (opcode == 0xF6 || opcode == 0xF7)
                {
                    if (operation != 2 && operation != 3)
                    {
                        return std::nullopt;
                    }
                    mnemonic = operation == 2 ? "not" : "neg";
                }
                else
                {
                    if (operation != 0 && operation != 1)
                    {
                        return std::nullopt;
                    }
                    mnemonic = operation == 0 ? "inc" : "dec";
                }
                requireGroupOperation  = true;
                expectedGroupOperation = operation;
            }
            else
            {
                return std::nullopt;
            }

            Operand operand;
            int     registerIndex = 0;
            if (!parseModRm(bytes, position, prefix, operand, registerIndex) || !operand.memory)
            {
                return std::nullopt;
            }
            if (requireGroupOperation && (registerIndex & 0x7) != expectedGroupOperation)
            {
                return std::nullopt;
            }

            std::string source;
            if (immediate)
            {
                std::uint64_t raw = 0;
                if (!readInteger(bytes, position, immediateSize, raw))
                {
                    return std::nullopt;
                }
                char buffer[32] {};
                std::snprintf(buffer, sizeof(buffer), "0x%llx", static_cast<unsigned long long>(raw));
                source = buffer;
            }
            else
            {
                const auto sourceWidth = (opcode == 0x88 || opcode == 0x86) ? std::uint32_t {1} : width;
                source                 = registerOperand(registerIndex, sourceWidth, prefix.rex);
            }

            candidate.store = true;
            candidate.width = (opcode == 0x88 || opcode == 0x86 || opcode == 0xC6 || opcode == 0x80 || opcode == 0xFE
                               || opcode == 0xF6)
                                ? 1
                                : width;
            candidate.text  = storeText(
                mnemonic, candidate.width, formatOperand(operand, prefix.address32 ? 4 : 8, prefix.rex), source);
            candidate.end = position;
            return candidate;
        }
    } // namespace

    std::vector<DisassembledInstruction>
    disassemble(std::span<const std::byte> bytes, std::uint64_t address, std::size_t maxInstructions)
    {
        std::vector<DisassembledInstruction> instructions;
        if (bytes.empty() || maxInstructions == 0)
        {
            return instructions;
        }
        if (maxInstructions > kMaxDisassemblyInstructions)
        {
            maxInstructions = kMaxDisassemblyInstructions;
        }

        ZydisDecoder decoder;
        if (ZYAN_FAILED(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)))
        {
            return instructions;
        }

        ZydisFormatter formatter;
        if (ZYAN_FAILED(ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL)))
        {
            return instructions;
        }

        std::size_t position = 0;
        while (position < bytes.size() && instructions.size() < maxInstructions)
        {
            ZydisDecodedInstruction                                  instruction {};
            std::array<ZydisDecodedOperand, ZYDIS_MAX_OPERAND_COUNT> operands {};
            const auto                                               status = ZydisDecoderDecodeFull(
                &decoder, bytes.data() + position, bytes.size() - position, &instruction, operands.data());

            DisassembledInstruction decoded;
            decoded.address = address + position;
            if (ZYAN_SUCCESS(status) && instruction.length != 0 && instruction.length <= bytes.size() - position)
            {
                decoded.bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(position),
                                     bytes.begin() + static_cast<std::ptrdiff_t>(position + instruction.length));

                std::array<char, 256> text {};
                if (ZYAN_SUCCESS(ZydisFormatterFormatInstruction(&formatter,
                                                                 &instruction,
                                                                 operands.data(),
                                                                 instruction.operand_count_visible,
                                                                 text.data(),
                                                                 text.size(),
                                                                 decoded.address,
                                                                 nullptr)))
                {
                    decoded.text = text.data();
                }
                else
                {
                    decoded.text = ".byte";
                    for (const auto value : decoded.bytes)
                    {
                        char buffer[8] {};
                        std::snprintf(buffer, sizeof(buffer), " 0x%02x", std::to_integer<unsigned>(value));
                        decoded.text += buffer;
                    }
                }
                position += instruction.length;
            }
            else
            {
                // A byte Zydis cannot decode is emitted verbatim instead of guessing a length or mnemonic.
                decoded.bytes.push_back(bytes[position]);
                char buffer[16] {};
                std::snprintf(buffer, sizeof(buffer), ".byte 0x%02x", std::to_integer<unsigned>(bytes[position]));
                decoded.text = buffer;
                ++position;
            }
            instructions.push_back(std::move(decoded));
        }
        return instructions;
    }

    std::optional<StoreInstruction> decodeStoreBefore(std::span<const std::byte> bytesBefore)
    {
        if (bytesBefore.empty())
        {
            return std::nullopt;
        }

        const auto firstStart =
            bytesBefore.size() > kMaxInstructionLength ? bytesBefore.size() - kMaxInstructionLength : std::size_t {0};
        std::optional<StoreInstruction> best;
        for (std::size_t start = firstStart; start < bytesBefore.size(); ++start)
        {
            const auto candidate = decodeOne(bytesBefore, start);
            if (!candidate || !candidate->store || candidate->end != bytesBefore.size())
            {
                continue;
            }
            // Prefer the longest decoding: it consumes REX and other prefixes that a shorter, accidental
            // decoding would drop.
            StoreInstruction instruction;
            instruction.offset = start;
            instruction.length = bytesBefore.size() - start;
            instruction.width  = candidate->width;
            instruction.text   = candidate->text;
            best               = instruction;
            break;
        }
        return best;
    }
} // namespace mpfshost::disasm
