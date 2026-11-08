#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <cctype>
#include <iomanip>

using namespace std;

struct HlibArgument
{
    string type;
    string name;
};

struct HlibFunction
{
    string name;
    vector<HlibArgument> args;
    vector<string> body;
};

struct HlibClassField
{
    string type;
    string name;
    bool stored = false; // *type: stored in every object, not a function argument
};

struct HlibClass
{
    string name;
    vector<HlibClassField> fields;
};

struct HlibObject
{
    string class_name;
    string name;
    vector<string> field_names;
    vector<string> metadata_names;
    vector<string> metadata_values;
};

void load_value(ostream&, const string&, const vector<string>&, const vector<string>&, const vector<unsigned char>&, const vector<unsigned char>&, const vector<unsigned long long>&, string);
void store_value(ostream&, const string&, unsigned, string);

struct HydList
{
    string name;
    unsigned bits = 8;
    unsigned length = 0;
};

int find_list(const vector<HydList>& lists, const string& name)
{
    for (int i = 0; i < (int)lists.size(); ++i)
        if (lists[i].name == name) return i;
    return -1;
}

bool parse_list_reference(const string& token, string& list_name, string& index)
{
    size_t colon = token.find(':');

    if (colon == string::npos)
        return false;

    list_name = token.substr(0, colon);
    index = token.substr(colon + 1);

    return !list_name.empty() && !index.empty();
}

string list_element_name(const string& name, unsigned index)
{
    return name + "__" + to_string(index);
}

void emit_list_load(
    ostream& out,
    const HydList& list,
    const string& index,
    const vector<string>& var_name,
    const vector<string>& const_name,
    const vector<unsigned char>& var_bits,
    const vector<unsigned char>& const_bits,
    const vector<unsigned long long>& const_value,
    unsigned& serial)
{
    load_value(
        out, index,
        var_name, const_name,
        var_bits, const_bits,
        const_value, "ecx"
    );

    string done = "_hyd_list_read_done_" + to_string(++serial);

    for (unsigned i = 0; i < list.length; ++i)
    {
        string label = "_hyd_list_read_" +
            to_string(serial) + "_" + to_string(i);

        out << "    cmp ecx, " << i << "\n";
        out << "    je " << label << "\n";
    }

    out << "    jmp list_index_error\n";

    for (unsigned i = 0; i < list.length; ++i)
    {
        out << "_hyd_list_read_" << serial << "_" << i << ":\n";

        load_value(
            out,
            list_element_name(list.name, i),
            var_name, const_name,
            var_bits, const_bits,
            const_value, "eax"
        );

        out << "    jmp " << done << "\n";
    }

    out << done << ":\n";
}

void emit_list_store(
    ostream& out,
    const HydList& list,
    const string& index,
    const vector<string>& var_name,
    const vector<string>& const_name,
    const vector<unsigned char>& var_bits,
    const vector<unsigned char>& const_bits,
    const vector<unsigned long long>& const_value,
    unsigned& serial)
{
    string done = "_hyd_list_write_done_" + to_string(++serial);

    out << "    push eax\n";

    load_value(
        out, index,
        var_name, const_name,
        var_bits, const_bits,
        const_value, "ecx"
    );

    for (unsigned i = 0; i < list.length; ++i)
    {
        string label = "_hyd_list_write_" +
            to_string(serial) + "_" + to_string(i);

        out << "    cmp ecx, " << i << "\n";
        out << "    je " << label << "\n";
    }

    out << "    jmp list_index_error\n";

    for (unsigned i = 0; i < list.length; ++i)
    {
        out << "_hyd_list_write_" << serial << "_" << i << ":\n";
        out << "    pop eax\n";

        store_value(
            out,
            list_element_name(list.name, i),
            list.bits,
            "eax"
        );

        out << "    jmp " << done << "\n";
    }

    out << done << ":\n";
}

struct HlibLibrary
{
    string name;
    vector<string> init_code;
    vector<string> data_code;
    vector<HlibClass> classes;
    vector<HlibFunction> functions;
};

int vector_find(const vector<string>& vec, const string& target)
{
    for (int i = 0; i < vec.size(); i++)
        if (vec[i] == target)
            return i;
    return -1;
}

string trim(string value)
{
    size_t first = value.find_first_not_of(" \t\r\n");
    if (first == string::npos) return "";
    size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

vector<string> split(string line)
{
    vector<string> result;
    string value;
    bool quoted = false;
    char quote = 0;
    for (size_t i = 0; i < line.size(); ++i)
    {
        char c = line[i];
        if ((c == '"' || c == '\'') && (!quoted || c == quote))
        {
            if (!quoted) { quoted = true; quote = c; }
            else quoted = false;
            value += c;
            continue;
        }
        if (!quoted && c == '>' && i + 1 < line.size() && line[i + 1] == '>')
            break;
        if (!quoted && isspace((unsigned char)c))
        {
            if (!value.empty()) { result.push_back(value); value.clear(); }
            continue;
        }
        if (!quoted && c == ';')
        {
            if (!value.empty()) { result.push_back(value); value.clear(); }
            result.push_back(";");
            continue;
        }
        value += c;
    }
    if (!value.empty()) result.push_back(value);
    return result;
}

bool parse_str_set_line(
    const string& line,
    string& destination,
    string& source)
{
    istringstream input(line);

    string command;
    if (!(input >> command >> destination) || command != "str_set")
        return false;

    getline(input, source);

    // Удаляем комментарий >>, только если он находится вне кавычек.
    bool quoted = false;
    char quote = 0;

    for (size_t i = 0; i + 1 < source.size(); ++i)
    {
        char c = source[i];

        if ((c == '"' || c == '\'') && (!quoted || c == quote))
        {
            if (!quoted)
            {
                quoted = true;
                quote = c;
            }
            else
                quoted = false;
        }

        if (!quoted && c == '>' && source[i + 1] == '>')
        {
            source.resize(i);
            break;
        }
    }

    source = trim(source);

    // Удаляем завершающую точку с запятой.
    if (!source.empty() && source.back() == ';')
        source.pop_back();

    source = trim(source);

    return !destination.empty() && !source.empty();
}

bool get_end(string arg, istringstream& code)
{
    if (!arg.empty() && arg.back() == ';') return true;
    string end;
    return (code >> end) && end == ";";
}

bool is_identifier(const string& value)
{
    if (value.empty()) return false;
    if (!(isalpha((unsigned char)value[0]) || value[0] == '_')) return false;
    for (char c : value)
        if (!(isalnum((unsigned char)c) || c == '_')) return false;
    return true;
}

bool is_number_token(const string& value)
{
    if (value.empty()) return false;

    if (isdigit((unsigned char)value[0]) ||
        value[0] == '-' || value[0] == '+')
        return true;

    return value.size() >= 2 &&
           value.front() == '\'' &&
           value.back() == '\'';
}


unsigned long long parseChar(const string& value)
{
    if (value.size() < 3 ||
        value.front() != '\'' ||
        value.back() != '\'')
    {
        throw runtime_error(
            "D014: Invalid character literal: " + value
        );
    }

    string s = value.substr(1, value.size() - 2);

    if (s.empty())
        throw runtime_error("D014: Empty character literal");

    const auto* bytes =
        reinterpret_cast<const unsigned char*>(s.data());

    size_t length = s.size();
    unsigned int cp = 0;
    size_t expected = 0;

    if (bytes[0] <= 0x7F)
    {
        cp = bytes[0];
        expected = 1;
    }
    else if (bytes[0] >= 0xC2 && bytes[0] <= 0xDF)
    {
        cp = bytes[0] & 0x1F;
        expected = 2;
    }
    else if (bytes[0] >= 0xE0 && bytes[0] <= 0xEF)
    {
        cp = bytes[0] & 0x0F;
        expected = 3;
    }
    else if (bytes[0] >= 0xF0 && bytes[0] <= 0xF4)
    {
        cp = bytes[0] & 0x07;
        expected = 4;
    }
    else
    {
        throw runtime_error("D014: Invalid UTF-8 character");
    }

    if (length != expected)
    {
        throw runtime_error(
            "D015: Character literal must contain exactly one Unicode character: "
            + value
        );
    }

    for (size_t i = 1; i < expected; ++i)
    {
        if ((bytes[i] & 0xC0) != 0x80)
            throw runtime_error("D016: Invalid UTF-8 sequence");

        cp = (cp << 6) | (bytes[i] & 0x3F);
    }

    if ((expected == 2 && cp < 0x80) ||
        (expected == 3 && cp < 0x800) ||
        (expected == 4 && cp < 0x10000) ||
        (cp >= 0xD800 && cp <= 0xDFFF) ||
        cp > 0x10FFFF)
    {
        throw runtime_error("D017: Invalid Unicode code point");
    }

    return cp;
}


unsigned long long get_number(string value)
{
    if (value.empty())
        throw runtime_error("D001: Expected integer");

    if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'')
    {
        return parseChar(value);
    }

    int type = 10;
    char suffix = (char)tolower((unsigned char)value.back());

    if (suffix == 'b' || suffix == 'o' || suffix == 'h' || suffix == 's')
    {
        value.pop_back();

        if (suffix == 'b') type = 2;
        else if (suffix == 'o') type = 8;
        else if (suffix == 'h') type = 16;
        else type = 10;
    }

    if (value.empty() || value == "-" || value == "+")
        throw runtime_error("D001: Expected integer");

    bool negative = value[0] == '-';
    size_t start = (value[0] == '-' || value[0] == '+') ? 1 : 0;

    for (size_t i = start; i < value.size(); i++)
    {
        unsigned char c = (unsigned char)value[i];
        bool valid = false;

        if (type == 2) valid = c == '0' || c == '1';
        else if (type == 8) valid = c >= '0' && c <= '7';
        else if (type == 10) valid = isdigit(c) != 0;
        else if (type == 16) valid = isxdigit(c) != 0;

        if (!valid)
            throw runtime_error("D002: Invalid integer literal: " + value);
    }

    if (type == 16 && value.size() > start &&
        isalpha((unsigned char)value[start]) &&
        value[start] >= 'A' && value[start] <= 'F')
    {
        throw runtime_error(
            "D003: Hexadecimal literals starting with A-F must begin with 0, e.g. 0FFh");
    }

    if (negative)
        return (unsigned long long)stoll(value, nullptr, type);

    return stoull(value, nullptr, type);
}



unsigned long long bit_mask(unsigned bits)
{
    if (bits >= 32) return 0xFFFFFFFFULL;
    if (bits == 0) return 0;
    return (1ULL << bits) - 1;
}

string storage_type(unsigned bits)
{
    if (bits <= 8) return "BYTE";
    if (bits <= 16) return "WORD";
    return "DWORD";
}

string low_register(unsigned bits, string reg = "eax")
{
    if (reg == "eax")
    {
        if (bits <= 8) return "al";
        if (bits <= 16) return "ax";
        return "eax";
    }
    if (reg == "ecx")
    {
        if (bits <= 8) return "cl";
        if (bits <= 16) return "cx";
        return "ecx";
    }
    throw runtime_error("I001: Internal register error");
}

string masm_number(unsigned long long value)
{
    value &= 0xFFFFFFFFULL;
    if (value <= 0x7FFFFFFFULL) return to_string(value);
    ostringstream out;
    out << "0" << uppercase << hex << value << "h";
    return out.str();
}

int mode_code(const string& mode)
{
    if (mode == "b") return 2;
    if (mode == "o") return 8;
    if (mode == "d") return 10;
    if (mode == "h") return 16;
    if (mode == "s") return 20;
    if (mode == "c") return 0;
    throw runtime_error("L001: Invalid mode; expected b, o, d, h, s or c");
}

int find_library_class(const HlibLibrary& library, const string& name)
{
    for (int i = 0; i < (int)library.classes.size(); ++i)
        if (library.classes[i].name == name) return i;
    return -1;
}

int find_library_function(const HlibLibrary& library, const string& name)
{
    for (int i = 0; i < (int)library.functions.size(); i++)
        if (library.functions[i].name == name) return i;
    return -1;
}

bool is_hlib_function_name(const string& value)
{
    if (!value.empty() && value.back() == '+')
        return is_identifier(value.substr(0, value.size() - 1));
    return is_identifier(value);
}

HlibLibrary load_hlib(string name)
{
    string path = "libs/" + name + ".hlib";
    ifstream file(path);
    if (!file)
    {
        file.clear();
        path = name + ".hlib";
        file.open(path);
    }
    if (!file) throw runtime_error("L002: Hlib file not found: " + name + ".hlib");

    HlibLibrary library;
    library.name = name;
    HlibFunction* current = nullptr;
    string line;
    vector<string> classes = {"var", "value", "num", "bool", "size", "name", "mode", "const", "cvalue"};

    enum class HlibSection { None, Init, Data, Class, Functions };
    HlibSection section = HlibSection::None;
    int section_order = 0;
    bool seen_init = false, seen_data = false, seen_class = false, seen_functions = false;

    auto enter_section = [&](const string& marker, int order, bool& seen, HlibSection next)
    {
        if (seen)
            throw runtime_error("L027: Duplicate " + marker + " section in " + name + ".hlib");
        if (order < section_order)
            throw runtime_error("L028: Hlib sections must be ordered HYDinit, HYDdata, HYDclass, HYDfunc in " + name + ".hlib");
        seen = true;
        section_order = order;
        section = next;
        current = nullptr;
    };

    bool first_line = true;

    while (getline(file, line))
    {
        if (first_line)
        {
            first_line = false;
            if (line.size() >= 3 &&
                (unsigned char)line[0] == 0xEF &&
                (unsigned char)line[1] == 0xBB &&
                (unsigned char)line[2] == 0xBF)
            {
                line.erase(0, 3);
            }
        }

        string clean = trim(line);

        if (clean == "HYDinit")
        {
            enter_section("HYDinit", 1, seen_init, HlibSection::Init);
            continue;
        }
        if (clean == "HYDdata")
        {
            enter_section("HYDdata", 2, seen_data, HlibSection::Data);
            continue;
        }
        if (clean == "HYDclass")
        {
            enter_section("HYDclass", 3, seen_class, HlibSection::Class);
            continue;
        }
        if (clean.rfind("HYDclass ", 0) == 0)
        {
            // Support both forms: a standalone HYDclass declaration, and
            // declarations grouped under a preceding HYDclass section marker.
            if (section != HlibSection::Class)
            {
                if (section_order > 3)
                    throw runtime_error("L028: Hlib sections must be ordered HYDinit, HYDdata, HYDclass, HYDfunc in " + name + ".hlib");
                section_order = 3;
                seen_class = true;
                section = HlibSection::Class;
                current = nullptr;
            }
            istringstream declaration(clean);
            string keyword, class_name, field_type, field_name;
            declaration >> keyword >> class_name;
            if (!is_identifier(class_name))
                throw runtime_error("L030: Invalid HYDclass name: " + class_name);
            if (find_library_class(library, class_name) != -1)
                throw runtime_error("L031: Duplicate HYDclass: " + class_name);
            HlibClass new_class;
            new_class.name = class_name;
            while (declaration >> field_type)
            {
                bool stored = !field_type.empty() && field_type.front() == '*';
                if (stored) field_type.erase(field_type.begin());
                if (!(declaration >> field_name))
                    throw runtime_error("L032: Missing field name in HYDclass " + class_name);
                if (!is_identifier(field_type) || !is_identifier(field_name))
                    throw runtime_error("L033: Invalid field declaration in HYDclass " + class_name);
                for (const auto& field : new_class.fields)
                    if (field.name == field_name)
                        throw runtime_error("L034: Duplicate field in HYDclass " + class_name + ": " + field_name);
                new_class.fields.push_back({field_type, field_name, stored});
            }
            if (new_class.fields.empty())
                throw runtime_error("L035: HYDclass must declare at least one field: " + class_name);
            library.classes.push_back(new_class);
            continue;
        }

        if (clean.rfind("HYDfunc", 0) == 0)
        {
            if (!seen_functions)
            {
                if (4 < section_order)
                    throw runtime_error("L028: Hlib sections must be ordered HYDinit, HYDdata, HYDclass, HYDfunc in " + name + ".hlib");
                section_order = 4;
                seen_functions = true;
            }
            section = HlibSection::Functions;

            istringstream declaration(clean);
            string keyword, function_name, arg_type, arg_name;
            declaration >> keyword >> function_name;
            if (keyword != "HYDfunc" || !is_hlib_function_name(function_name))
                throw runtime_error("L003: Invalid HYDfunc declaration in " + name + ".hlib");
            if (find_library_function(library, function_name) != -1)
                throw runtime_error("L004: Duplicate HYDfunc: " + function_name);

            HlibFunction function;
            function.name = function_name;
            while (declaration >> arg_type)
            {
                if (!(declaration >> arg_name))
                    throw runtime_error("L005: Missing argument name in HYDfunc " + function_name);
                if (vector_find(classes, arg_type) == -1 && find_library_class(library, arg_type) == -1)
                    throw runtime_error("L006: Unknown argument class: " + arg_type);
                if (!is_identifier(arg_name))
                    throw runtime_error("Invalid argument name: " + arg_name);
                for (const auto& arg : function.args)
                    if (arg.name == arg_name) throw runtime_error("L008: Duplicate argument name: " + arg_name);
                function.args.push_back({arg_type, arg_name});
            }
            library.functions.push_back(function);
            current = &library.functions.back();
            continue;
        }

        switch (section)
        {
        case HlibSection::Init:
            library.init_code.push_back(line);
            break;
        case HlibSection::Data:
            library.data_code.push_back(line);
            break;
        case HlibSection::Class:
            break;
        case HlibSection::Functions:
            if (current) current->body.push_back(line);
            break;
        case HlibSection::None:
            if (!clean.empty())
                throw runtime_error("L009: Content before first Hlib section in " + name + ".hlib");
            break;
        }
    }

    if (library.functions.empty()) throw runtime_error("L010: No HYDfunc declarations in " + name + ".hlib");
    return library;
}

int find_symbol(const vector<string>& names, const string& name)
{
    return vector_find(names, name);
}

unsigned long long get_value(const string& value,
    const vector<string>& var_name, const vector<string>& const_name,
    const vector<unsigned char>& var_bits, const vector<unsigned char>& const_bits,
    const vector<unsigned long long>& var_value, const vector<unsigned long long>& const_value,
    bool allow_variable = true)
{
    if (is_number_token(value)) return get_number(value);
    int var = find_symbol(var_name, value);
    int con = find_symbol(const_name, value);
    if (var == -1 && con == -1) throw runtime_error("D004: Unknown variable or constant: " + value);
    if (!allow_variable && con == -1) throw runtime_error("D005: Expected constant: " + value);
    if (con != -1) return const_value[con];
    (void)var_bits;
    (void)const_bits;
    return allow_variable ? var_value[var] : 0;
}

void load_value(ostream& out, const string& value,
    const vector<string>& var_name, const vector<string>& const_name,
    const vector<unsigned char>& var_bits, const vector<unsigned char>& const_bits,
    const vector<unsigned long long>& const_value, string reg = "eax")
{
    if (is_number_token(value))
    {
        out << "    mov " << reg << ", " << masm_number(get_number(value)) << "\n";
        return;
    }

    int con = find_symbol(const_name, value);
    if (con != -1)
    {
        out << "    mov " << reg << ", " << masm_number(const_value[con]) << "\n";
        return;
    }

    int var = find_symbol(var_name, value);
    if (var == -1) throw runtime_error("D004: Unknown variable or constant: " + value);
    string storage = storage_type(var_bits[var]);
    if (reg == "eax")
    {
        if (storage == "BYTE") out << "    movzx eax, BYTE PTR [" << value << "]\n";
        else if (storage == "WORD") out << "    movzx eax, WORD PTR [" << value << "]\n";
        else out << "    mov eax, DWORD PTR [" << value << "]\n";
    }
    else if (reg == "ecx")
    {
        if (storage == "BYTE") out << "    movzx ecx, BYTE PTR [" << value << "]\n";
        else if (storage == "WORD") out << "    movzx ecx, WORD PTR [" << value << "]\n";
        else out << "    mov ecx, DWORD PTR [" << value << "]\n";
    }
    else throw runtime_error("I002: Unsupported register: " + reg);
    if (var_bits[var] < 32)
        out << "    and " << reg << ", " << masm_number(bit_mask(var_bits[var])) << "\n";
    (void)const_bits;
}

void store_value(ostream& out, const string& name, unsigned bits, string reg = "eax")
{
    if (bits < 32) out << "    and " << reg << ", " << masm_number(bit_mask(bits)) << "\n";
    out << "    mov " << storage_type(bits) << " PTR [" << name << "], " << low_register(bits, reg) << "\n";
}

string make_load_code(const string& value,
    const vector<string>& var_name, const vector<string>& const_name,
    const vector<unsigned char>& var_bits, const vector<unsigned char>& const_bits,
    const vector<unsigned long long>& const_value)
{
    ostringstream out;
    load_value(out, value, var_name, const_name, var_bits, const_bits, const_value);
    return out.str();
}

string make_signed_load_code(const string& value,
    const vector<string>& var_name, const vector<string>& const_name,
    const vector<unsigned char>& var_bits, const vector<unsigned char>& const_bits,
    const vector<unsigned long long>& const_value)
{
    ostringstream out;
    if (is_number_token(value))
    {
        load_value(out, value, var_name, const_name, var_bits, const_bits, const_value);
        return out.str();
    }
    int con = find_symbol(const_name, value);
    if (con != -1)
    {
        out << "    mov eax, " << masm_number(const_value[con]) << "\n";
        unsigned bits = const_bits[con];
        if (bits < 32) out << "    shl eax, " << (32 - bits) << "\n    sar eax, " << (32 - bits) << "\n";
        return out.str();
    }
    int var = find_symbol(var_name, value);
    if (var == -1) throw runtime_error("D004: Unknown variable or constant: " + value);
    unsigned bits = var_bits[var];
    string storage = storage_type(bits);
    if (bits <= 8) out << "    movsx eax, BYTE PTR [" << value << "]\n";
    else if (bits <= 16) out << "    movsx eax, WORD PTR [" << value << "]\n";
    else out << "    mov eax, DWORD PTR [" << value << "]\n";
    (void)storage;
    (void)const_bits;
    return out.str();
}

bool is_identifier_char(char c)
{
    unsigned char u = (unsigned char)c;
    return isalnum(u) || c == '_';
}

string replace_label_references(string line, const vector<string>& label_names,
    const vector<string>& unique_names)
{
    for (size_t i = 0; i < label_names.size(); i++)
    {
        size_t pos = 0;
        while ((pos = line.find(label_names[i], pos)) != string::npos)
        {
            size_t end = pos + label_names[i].size();
            bool left_ok = pos == 0 || !is_identifier_char(line[pos - 1]);
            bool right_ok = end == line.size() || !is_identifier_char(line[end]);
            if (left_ok && right_ok)
            {
                line.replace(pos, label_names[i].size(), unique_names[i]);
                pos += unique_names[i].size();
            }
            else pos = end;
        }
    }
    return line;
}

void compile_library_call(ostream& out, const string& alias, const vector<string>& tokens,
    const vector<string>& libs_name, const vector<string>& libs_alias,
    const vector<HlibLibrary>& libs,
    const vector<string>& var_name, const vector<unsigned char>& var_bits,
    const vector<string>& const_name, const vector<unsigned char>& const_bits,
    const vector<unsigned long long>& const_value,
    const vector<HlibObject>& objects)
{
    int lib_index = vector_find(libs_alias, alias);
    if (lib_index == -1) throw runtime_error("L011: Unknown library alias: " + alias);
    if (tokens.empty()) throw runtime_error("L012: Expected library function name");
    int function_index = find_library_function(libs[lib_index], tokens[0]);
    if (function_index == -1) throw runtime_error("L013: Unknown library function: " + tokens[0]);
    const HlibFunction& function = libs[lib_index].functions[function_index];
    if ((int)tokens.size() - 1 != (int)function.args.size())
        throw runtime_error("L014: Wrong argument count for library function: " + function.name);

    static unsigned long long next_hlib_call_id = 0;
    unsigned long long call_id = ++next_hlib_call_id;
    vector<string> label_names, unique_label_names;

    for (const string& source_line : function.body)
    {
        if (source_line.empty() || isspace((unsigned char)source_line[0])) continue;

        string label = trim(source_line);
        if (!label.empty() && label.back() == ':') label.pop_back();
        if (!is_identifier(label))
            throw runtime_error("L015: An unindented .hlib line must be a label ending with ':' in function " + function.name + ": " + source_line);
        if (vector_find(label_names, label) != -1)
            throw runtime_error("L016: Duplicate label in HYDfunc " + function.name + ": " + label);

        label_names.push_back(label);
        unique_label_names.push_back("_hyd_lbl_" + to_string(call_id) + "_" + label);
    }

    vector<string> replacement_names, replacement_values;
    for (size_t i = 0; i < function.args.size(); i++)
    {
        const HlibArgument& arg = function.args[i];
        const string& value = tokens[i + 1];
        int var = find_symbol(var_name, value);
        int con = find_symbol(const_name, value);
        bool numeric = is_number_token(value);
        int object_index = -1;
        const HlibClass* object_class = nullptr;
        if (find_library_class(libs[lib_index], arg.type) != -1)
        {
            for (int oi = 0; oi < (int)objects.size(); ++oi)
                if (objects[oi].name == value && objects[oi].class_name == arg.type)
                    object_index = oi;
            if (object_index == -1)
                throw runtime_error("L036: Argument " + arg.name + " must be an object of class " + arg.type + ": " + value);
            for (const auto& candidate_library : libs)
            {
                int ci = find_library_class(candidate_library, arg.type);
                if (ci != -1) { object_class = &candidate_library.classes[ci]; break; }
            }
        }

        if (arg.type == "var" && (var == -1 || con != -1)) throw runtime_error("L018: Argument " + arg.name + " must be a variable");
        if (arg.type == "const" && con == -1) throw runtime_error("L019: Argument " + arg.name + " must be a constant");
        if (arg.type == "value" && var == -1 && con == -1 && !numeric) throw runtime_error("L020: Argument " + arg.name + " must be a variable, constant or number");
        if (arg.type == "num" && !numeric) throw runtime_error("L021: Argument " + arg.name + " must be a number");
        if (arg.type == "cvalue" && con == -1 && !numeric) throw runtime_error("L022: Argument " + arg.name + " must be a constant or number");
        if (arg.type == "bool" && value != "0" && value != "1") throw runtime_error("L023: Argument " + arg.name + " must be 0 or 1");
        if (arg.type == "size")
        {
            if (!numeric) throw runtime_error("L024: Argument " + arg.name + " must be a size from 0 to 32");
            unsigned long long n = get_number(value);
            if (n > 32) throw runtime_error("L025: Argument " + arg.name + " must be from 0 to 32");
        }
        if (arg.type == "mode") mode_code(value);
        if (arg.type == "name" && value.empty()) throw runtime_error("L026: Argument " + arg.name + " must be a name");
        if ((arg.type == "num" || arg.type == "value" || arg.type == "cvalue") && numeric) get_number(value);

        string prefix = arg.name;
        replacement_names.push_back("{" + prefix + "}");
        replacement_values.push_back(value);
        replacement_names.push_back("{" + prefix + "_load}");
        replacement_values.push_back((arg.type == "var" || arg.type == "value" || arg.type == "const" || arg.type == "cvalue" || arg.type == "num")
            ? make_load_code(value, var_name, const_name, var_bits, const_bits, const_value) : "");
        replacement_names.push_back("{" + prefix + "_sload}");
        replacement_values.push_back((arg.type == "var" || arg.type == "value" || arg.type == "const" || arg.type == "cvalue" || arg.type == "num")
            ? make_signed_load_code(value, var_name, const_name, var_bits, const_bits, const_value) : "");
        replacement_names.push_back("{" + prefix + "_bits}");
        replacement_values.push_back(var != -1 ? to_string(var_bits[var]) : con != -1 ? to_string(const_bits[con]) : "32");
        replacement_names.push_back("{" + prefix + "_storage}");
        replacement_values.push_back(var != -1 ? storage_type(var_bits[var]) : con != -1 ? storage_type(const_bits[con]) : "DWORD");
        replacement_names.push_back("{" + prefix + "_register}");
        replacement_values.push_back(var != -1 ? low_register(var_bits[var]) : con != -1 ? low_register(const_bits[con]) : "eax");
        replacement_names.push_back("{" + prefix + "_mask}");
        replacement_values.push_back(var != -1 ? masm_number(bit_mask(var_bits[var])) : con != -1 ? masm_number(bit_mask(const_bits[con])) : "0FFFFFFFFh");
        replacement_names.push_back("{" + prefix + "_code}");
        replacement_values.push_back(arg.type == "mode" ? to_string(mode_code(value)) : value);

        if (object_index != -1 && object_class)
        {
            const HlibObject& object = objects[object_index];
            for (const auto& field : object_class->fields)
            {
                string field_prefix = "{" + prefix + ">" + field.name;
                if (!field.stored)
                {
                    auto metadata = find(object.metadata_names.begin(), object.metadata_names.end(), field.name);
                    if (metadata == object.metadata_names.end())
                        throw runtime_error("I006: Missing metadata for class field " + value + ">" + field.name);
                    size_t metadata_index = (size_t)(metadata - object.metadata_names.begin());
                    replacement_names.push_back(field_prefix + "}");
                    replacement_values.push_back(object.metadata_values[metadata_index]);
                    continue;
                }

                string field_storage;
                for (const string& actual_field : object.field_names)
                    if (actual_field == value + "_" + field.name)
                        field_storage = actual_field;
                if (field_storage.empty())
                    throw runtime_error("I005: Missing storage for class field " + value + ">" + field.name);
                replacement_names.push_back(field_prefix + "}");
                replacement_values.push_back(field_storage);
                replacement_names.push_back(field_prefix + "_load}");
                int field_var = find_symbol(var_name, field_storage);
                replacement_values.push_back(field_var == -1 ? "" : make_load_code(field_storage, var_name, const_name, var_bits, const_bits, const_value));
                replacement_names.push_back(field_prefix + "_sload}");
                replacement_values.push_back(field_var == -1 ? "" : make_signed_load_code(field_storage, var_name, const_name, var_bits, const_bits, const_value));
                replacement_names.push_back(field_prefix + "_bits}");
                replacement_values.push_back(field_var == -1 ? "32" : to_string(var_bits[field_var]));
                replacement_names.push_back(field_prefix + "_storage}");
                replacement_values.push_back(field_var == -1 ? "DWORD" : storage_type(var_bits[field_var]));
                replacement_names.push_back(field_prefix + "_register}");
                replacement_values.push_back(field_var == -1 ? "eax" : low_register(var_bits[field_var]));
                replacement_names.push_back(field_prefix + "_mask}");
                replacement_values.push_back(field_var == -1 ? "0FFFFFFFFh" : masm_number(bit_mask(var_bits[field_var])));
            }
        }
    }

    for (const string& source_line : function.body)
    {
        if (source_line.empty())
        {
            out << "\n";
            continue;
        }

        if (!isspace((unsigned char)source_line[0]))
        {
            string label = trim(source_line);
            if (!label.empty() && label.back() == ':') label.pop_back();
            int label_index = vector_find(label_names, label);
            if (label_index == -1) throw runtime_error("I003: Internal .hlib label error: " + label);
            out << unique_label_names[label_index] << ":\n";
            continue;
        }

        string output_line = source_line;
        for (size_t i = 0; i < replacement_names.size(); i++)
        {
            size_t pos = 0;
            while ((pos = output_line.find(replacement_names[i], pos)) != string::npos)
            {
                output_line.replace(pos, replacement_names[i].size(), replacement_values[i]);
                pos += replacement_values[i].size();
            }
        }
        output_line = replace_label_references(output_line, label_names, unique_label_names);
        if (output_line.find('{') != string::npos || output_line.find('}') != string::npos)
            throw runtime_error("L017: Unknown or malformed placeholder in " + libs[lib_index].name + ".hlib");
        out << output_line << "\n";
    }
    (void)libs_name;
}

struct ConstructionBlock
{
    string type = "if";
    string false_label;
    string end_label;
    bool has_else = false;
    unsigned branch_number = 0;
    string counter_name;
    string counter_storage;
    string bound_storage;
    string loop_label;
    bool descending = false;
};

struct ActiveCounter
{
    string name;
    string storage;
};

int find_active_counter(const vector<ActiveCounter>& counters, const string& name)
{
    for (int i = (int)counters.size() - 1; i >= 0; --i)
        if (counters[i].name == name) return i;
    return -1;
}

string resolve_counter_name(const vector<ActiveCounter>& counters, const string& name)
{
    int index = find_active_counter(counters, name);
    return index == -1 ? name : counters[index].storage;
}

unsigned value_bits(unsigned long long value)
{
    unsigned bits = 1;
    while (bits < 32 && value > bit_mask(bits)) ++bits;
    return bits;
}

unsigned operand_bits(const string& operand,
    const vector<string>& var_name, const vector<unsigned char>& var_bits,
    const vector<string>& const_name, const vector<unsigned char>& const_bits,
    const vector<unsigned long long>& const_value)
{
    if (is_number_token(operand)) return value_bits(get_number(operand));
    int con = find_symbol(const_name, operand);
    if (con != -1) return value_bits(const_value[con]);
    int var = find_symbol(var_name, operand);
    if (var != -1) return var_bits[var];
    throw runtime_error("P003: Unknown variable or constant: " + operand);
}

void compile_construction(ostream& out, const vector<string>& tokens,
    const vector<string>& var_name, const vector<unsigned char>& var_bits,
    const vector<string>& const_name, const vector<unsigned char>& const_bits,
    const vector<unsigned long long>& const_value,
    vector<ConstructionBlock>& construction_stack,
    unsigned& next_construction_id)
{
    if (tokens.empty()) return;

    string cons = tokens[0];
    if (!cons.empty() && cons.back() == ':')
        cons.pop_back();

    if (cons != "if" && cons != "elseif" && cons != "else")
        throw runtime_error("P012: Construction not implemented yet: " + cons);

    if (tokens.back().empty() || tokens.back().back() != ':')
        throw runtime_error("P011: Expected ':' after construction: " + cons);

    vector<string> args(tokens.begin() + 1, tokens.end());
    if (!args.empty() && !args.back().empty() && args.back().back() == ':')
        args.back().pop_back();

    if (cons == "if")
    {
        if (args.size() != 1)
            throw runtime_error("P013: if expects one condition variable");

        unsigned id = ++next_construction_id;
        ConstructionBlock block;
        block.type = "if";
        block.false_label = "_hyd_if_false_" + to_string(id) + "_0";
        block.end_label = "_hyd_if_end_" + to_string(id);

        load_value(out, args[0], var_name, const_name,
            var_bits, const_bits, const_value);
        out << "    test eax, eax\n";
        out << "    jz " << block.false_label << "\n";

        construction_stack.push_back(block);
        return;
    }

    if (construction_stack.empty())
        throw runtime_error("P014: '" + cons + "' without matching if");

    ConstructionBlock& block = construction_stack.back();
    if (block.type != "if")
        throw runtime_error("P014: '" + cons + "' without matching if");

    if (cons == "elseif")
    {
        if (block.has_else)
            throw runtime_error("P015: elseif cannot appear after else");
        if (args.size() != 1)
            throw runtime_error("P016: elseif expects one condition variable");

        out << "    jmp " << block.end_label << "\n";
        out << block.false_label << ":\n";

        ++block.branch_number;
        block.false_label = "_hyd_if_false_" +
            block.end_label.substr(string("_hyd_if_end_").size()) + "_" +
            to_string(block.branch_number);

        load_value(out, args[0], var_name, const_name,
            var_bits, const_bits, const_value);
        out << "    test eax, eax\n";
        out << "    jz " << block.false_label << "\n";
        return;
    }

    if (!args.empty())
        throw runtime_error("P017: else does not take a condition");
    if (block.has_else)
        throw runtime_error("P018: Duplicate else");

    out << "    jmp " << block.end_label << "\n";
    out << block.false_label << ":\n";
    block.has_else = true;
}

void compile_instruction(ostream& out, const vector<string>& tokens,
    const vector<string>& var_name, const vector<unsigned char>& var_bits,
    const vector<string>& const_name, const vector<unsigned char>& const_bits,
    const vector<unsigned long long>& const_value,
    const vector<string>& libs_name, const vector<string>& libs_alias,
    const vector<HlibLibrary>& libs, const vector<HlibObject>& objects)
{
    if (tokens.empty()) return;
    string func = tokens[0];
    if (tokens.back() != ";") throw runtime_error("P001: Expected ';' after instruction: " + func);
    vector<string> args(tokens.begin() + 1, tokens.end() - 1);

    if (func.size() >= 3 && func.front() == '<' && func.back() == '>')
    {
        compile_library_call(out, func.substr(1, func.size() - 2), args,
            libs_name, libs_alias, libs, var_name, var_bits, const_name, const_bits, const_value, objects);
        return;
    }

    if (func == "debug" || func == "not")
    {
        if (args.size() != 1) throw runtime_error("P002: " + func + " expects 1 argument");
        int var = find_symbol(var_name, args[0]);
        if (var == -1) throw runtime_error("P003: Unknown variable: " + args[0]);
        load_value(out, args[0], var_name, const_name, var_bits, const_bits, const_value);
        if (func == "not")
        {
            out << "    not eax\n";
            store_value(out, args[0], var_bits[var], "eax");
        }
        else out << "    invoke DebugNumber, eax, " << (int)var_bits[var] << "\n";
        return;
    }

    if (func == "bit")
    {
        if (args.size() != 3) throw runtime_error("P004: bit expects 3 arguments");
        int var = find_symbol(var_name, args[0]);
        if (var == -1) throw runtime_error("P003: Unknown variable: " + args[0]);
        unsigned long long point = get_number(args[1]);
        if (point < 1 || point > var_bits[var]) throw runtime_error("P005: Bit position out of range");
        if (args[2] != "0" && args[2] != "1") throw runtime_error("P006: Bit value must be 0 or 1");
        load_value(out, args[0], var_name, const_name, var_bits, const_bits, const_value);
        out << "    " << (args[2] == "1" ? "bts" : "btr") << " eax, " << (point - 1) << "\n";
        store_value(out, args[0], var_bits[var]);
        return;
    }

    if (func == "cmp")
    {
        if (args.size() != 4)
            throw runtime_error("P010: cmp expects 4 arguments");

        int var1 = find_symbol(var_name, args[0]);
        int var2 = find_symbol(var_name, args[1]);
        int out_var = find_symbol(var_name, args[2]);

        if (var1 == -1)
            throw runtime_error("P003: Unknown variable: " + args[0]);

        if (var2 == -1)
            throw runtime_error("P003: Unknown variable: " + args[1]);

        if (out_var == -1)
            throw runtime_error("P003: Unknown variable: " + args[2]);

        if (args[3] != "0" && args[3] != "1")
            throw runtime_error("P006: cmp mode must be 0 or 1");

        load_value(out, args[0], var_name, const_name,
            var_bits, const_bits, const_value);

        out << "    push eax\n";

        load_value(out, args[1], var_name, const_name,
            var_bits, const_bits, const_value);

        out << "    mov ecx, eax\n";
        out << "    pop eax\n";
        out << "    cmp eax, ecx\n";

        if (args[3] == "0")
            out << "    sete al\n";
        else
            out << "    seta al\n";

        out << "    movzx eax, al\n";

        store_value(out, args[2], var_bits[out_var]);

        return;
    }

    vector<string> arithmetic = {
        "set",
        "add", "sub", "mul", "div", "mod",
        "s_add", "s_sub", "s_mul", "s_div", "s_mod",
        "and", "or", "xor"
    };
    if (vector_find(arithmetic, func) == -1) throw runtime_error("P007: Unknown instruction: " + func);
    if (args.size() != 2) throw runtime_error("P008: " + func + " expects 2 arguments");
    int var = find_symbol(var_name, args[0]);
    if (var == -1) throw runtime_error("P009: Destination must be a declared variable: " + args[0]);

        if (func == "set")
    {
        load_value(out, args[1], var_name, const_name,
            var_bits, const_bits, const_value);
    }
    else
    {
        load_value(out, args[0], var_name, const_name,
            var_bits, const_bits, const_value);

        load_value(out, args[1], var_name, const_name,
            var_bits, const_bits, const_value, "ecx");

        if (func == "add" || func == "s_add")
        {
            out << "    add eax, ecx\n";
        }
        else if (func == "sub" || func == "s_sub")
        {
            out << "    sub eax, ecx\n";
        }
        else if (func == "mul")
        {
            out << "    mul ecx\n";
        }
        else if (func == "s_mul")
        {
            out << "    imul eax, ecx\n";
        }
        else if (func == "div" || func == "mod")
        {
            out << "    test ecx, ecx\n"
                << "    jz division_error\n"
                << "    xor edx, edx\n"
                << "    div ecx\n";

            if (func == "mod")
                out << "    mov eax, edx\n";
        }
        else if (func == "s_div" || func == "s_mod")
        {
            out << "    test ecx, ecx\n"
                << "    jz division_error\n"
                << "    cdq\n"
                << "    idiv ecx\n";

            if (func == "s_mod")
                out << "    mov eax, edx\n";
        }
        else if (func == "and")
        {
            out << "    and eax, ecx\n";
        }
        else if (func == "or")
        {
            out << "    or eax, ecx\n";
        }
        else if (func == "xor")
        {
            out << "    xor eax, ecx\n";
        }
    }

    store_value(out, args[0], var_bits[var]);
}

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        cerr << "H001: Usage: hydronic <file.hyd>\n";
        return 1;
    }

    ifstream hydp(argv[1]);
    if (!hydp)
    {
        cerr << "H002: Cannot open <" << argv[1] << ">\n";
        return 1;
    }

    vector<string> var_name, const_name;
    vector<unsigned char> var_bits, const_bits;
    vector<unsigned long long> var_value, const_value;
    vector<string> libs_name, libs_alias;
    vector<HlibLibrary> libs;
    vector<HlibObject> objects;
    vector<HydList> lists;
    vector<string> code;

    bool section[] = {false, false, false, false};
    bool foundMain = false;
    string line;

    try
    {
        while (getline(hydp, line))
        {
            string clean = trim(line);
            if (clean.empty() || clean[0] == '>') continue;
            vector<string> tokens = split(clean);
            if (tokens.empty()) continue;
            string func = tokens[0];

            if (func == "/lib")
            {
                if (section[0] || section[1] || section[2]) throw runtime_error("S001: Invalid /lib section");
                section[3] = true;
                continue;
            }
            if (func == "/data")
            {
                if (section[1] || section[2]) throw runtime_error("S002: Invalid /data section");
                section[0] = true;
                section[3] = false;
                continue;
            }
            if (func == "/code")
            {
                if (section[2]) throw runtime_error("S003: Invalid /code section");
                section[1] = true;
                section[0] = false;
                section[3] = false;
                continue;
            }
            if (func == "\\main")
            {
                if (!section[1] || foundMain) throw runtime_error("S004: Invalid main section");
                foundMain = true;
                continue;
            }
            if (func == "/end")
            {
                if (!section[1] || !foundMain || section[2]) throw runtime_error("S005: Invalid /end section");
                section[2] = true;
                section[1] = false;
                continue;
            }
            if (section[2]) throw runtime_error("S006: Unexpected code after /end");

            if (section[3])
            {
                if (func != "lib" || tokens.size() != 4 || tokens.back() != ";")
                    throw runtime_error("Expected: lib library_name alias;");
                string name = tokens[1], alias = tokens[2];
                if (!is_identifier(alias)) throw runtime_error("S010: Invalid library alias: " + alias);
                if (vector_find(libs_alias, alias) != -1) throw runtime_error("S011: Duplicate library alias: " + alias);
                HlibLibrary library = load_hlib(name);
                libs_name.push_back(name);
                libs.push_back(library);
                libs_alias.push_back(alias);
                continue;
            }

            if (section[0])
            {
                if (tokens.back() != ";") throw runtime_error("D006: Expected ';' in data declaration");
                if (func.size() >= 3 && func.front() == '<' && func.back() == '>')
                {
                    string alias = func.substr(1, func.size() - 2);
                    int li = vector_find(libs_alias, alias);
                    if (li == -1) throw runtime_error("L011: Unknown library alias: " + alias);
                    if (tokens.size() < 4) throw runtime_error("D018: Invalid class object declaration: " + clean);
                    string class_name = tokens[1];
                    int ci = find_library_class(libs[li], class_name);
                    if (ci == -1) throw runtime_error("D019: Unknown class " + class_name + " in library " + alias);
                    string object_name = tokens[2];
                    if (!is_identifier(object_name)) throw runtime_error("D020: Invalid object name: " + object_name);
                    for (const auto& obj : objects)
                        if (obj.name == object_name) throw runtime_error("D021: Duplicate object name: " + object_name);
                    const HlibClass& cls = libs[li].classes[ci];
                    vector<const HlibClassField*> public_fields;
                    for (const auto& field : cls.fields) if (field.stored) public_fields.push_back(&field);
                    // The first class field(s) that are not stored are constructor arguments.
                    vector<const HlibClassField*> constructor_fields;
                    // A field declared as "name title" takes the object's identifier
                    // as its value, so it is not supplied again after the object name.
                    for (const auto& field : cls.fields)
                        if (!field.stored && field.type != "name") constructor_fields.push_back(&field);
                    size_t supplied = tokens.size() - 4; // after <alias> class object ... ;
                    if (supplied != constructor_fields.size())
                        throw runtime_error("D022: Class " + class_name + " expects " + to_string(constructor_fields.size()) + " constructor arguments");
                    HlibObject object;
                    object.class_name = class_name;
                    object.name = object_name;
                    size_t constructor_index = 0;
                    for (const auto& field : cls.fields)
                    {
                        if (!field.stored)
                        {
                            object.metadata_names.push_back(field.name);
                            if (field.type == "name")
                                object.metadata_values.push_back(object_name);
                            else
                            {
                                if (constructor_index >= constructor_fields.size())
                                    throw runtime_error("D022: Missing constructor value for class field " + field.name);
                                object.metadata_values.push_back(tokens[3 + constructor_index]);
                                ++constructor_index;
                            }
                            continue;
                        }
                        string field_name = object_name + "_" + field.name;
                        unsigned bits = 32;
                        if (field.name == "int" && !constructor_fields.empty())
                        {
                            // For real: the size argument named int_size controls the int field width.
                            auto it = find_if(constructor_fields.begin(), constructor_fields.end(), [](const HlibClassField* f) { return f->name == "int_size"; });
                            if (it != constructor_fields.end())
                            {
                                size_t idx = (size_t)(it - constructor_fields.begin());
                                unsigned long long b = get_number(tokens[3 + idx]);
                                if (b < 1 || b > 32) throw runtime_error("D023: Class integer field size must be 1..32");
                                bits = (unsigned)b;
                            }
                        }
                        var_name.push_back(field_name);
                        var_bits.push_back((unsigned char)bits);
                        var_value.push_back(0);
                        object.field_names.push_back(field_name);
                    }
                    objects.push_back(object);
                }
                else if (func == "list" && tokens.size() == 5)
                {
                    string name = tokens[1];
                    if (!is_identifier(name)) throw runtime_error("D024: Invalid list name: " + name);
                    if (vector_find(var_name, name) != -1 || vector_find(const_name, name) != -1 || find_list(lists, name) != -1)
                        throw runtime_error("D025: Duplicate list, variable or constant name: " + name);
                    unsigned long long bits = get_number(tokens[2]);
                    unsigned long long length = get_number(tokens[3]);
                    if (bits < 1 || bits > 32) throw runtime_error("D026: List element size must be 1..32");
                    if (length < 1 || length > 65535) throw runtime_error("D027: List length must be 1..65535");
                    HydList list; list.name = name; list.bits = (unsigned)bits; list.length = (unsigned)length;
                    lists.push_back(list);
                    for (unsigned i = 0; i < list.length; ++i)
                    {
                        string element = list_element_name(name, i);
                        var_name.push_back(element);
                        var_bits.push_back((unsigned char)bits);
                        var_value.push_back(0);
                    }
                }
                else if (func == "var" && tokens.size() == 4)
                {
                    string name = tokens[1];
                    if (!is_identifier(name)) throw runtime_error("D007: Invalid variable name: " + name);
                    if (vector_find(var_name, name) != -1 || vector_find(const_name, name) != -1 || find_list(lists, name) != -1) throw runtime_error("D009: Duplicate variable, constant or list name: " + name);
                    unsigned long long bits = get_number(tokens[2]);
                    if (bits < 1 || bits > 32) throw runtime_error("D010: Variable size must be 1..32");
                    var_name.push_back(name);
                    var_bits.push_back((unsigned char)bits);
                    var_value.push_back(0);
                }
                else if (func == "const" && tokens.size() == 5)
                {
                    string name = tokens[1];
                    if (!is_identifier(name)) throw runtime_error("D008: Invalid constant name: " + name);
                    if (vector_find(var_name, name) != -1 || vector_find(const_name, name) != -1 || find_list(lists, name) != -1) throw runtime_error("D009: Duplicate variable, constant or list name: " + name);
                    unsigned long long bits = get_number(tokens[2]);
                    unsigned long long value = get_number(tokens[3]);
                    if (bits < 1 || bits > 32) throw runtime_error("D011: Constant size must be 1..32");
                    if (value > bit_mask((unsigned)bits)) throw runtime_error("D012: Constant does not fit declared size");
                    const_name.push_back(name);
                    const_bits.push_back((unsigned char)bits);
                    const_value.push_back(value);
                }
                else throw runtime_error("D013: Invalid data declaration: " + clean);
                continue;
            }

            if (section[1] && foundMain)
            {
                code.push_back(clean);
                continue;
            }
            throw runtime_error("Code outside a valid section: " + clean);
        }

        if (!section[2]) throw runtime_error("S007: Missing /end");

        ofstream asmp("program.asm");
        if (!asmp) throw runtime_error("H003: Cannot create program.asm");
        asmp << ".386\n.model flat, stdcall\noption casemap:none\n\n";
        asmp << "includelib kernel32.lib\n";
        asmp << "DebugNumber PROTO :DWORD, :DWORD\n";
        asmp << "ExitProcess PROTO :DWORD\n\n";

        for (const HlibLibrary& library : libs)
        {
            for (const string& init_line : library.init_code)
                asmp << init_line << "\n";
        }

        ostringstream generated_code;
        vector<ConstructionBlock> construction_stack;
        vector<ActiveCounter> active_counters;
        unsigned next_construction_id = 0;
        unsigned list_codegen_serial = 0;

        for (const string& instruction : code)
        {
            vector<string> tokens = split(instruction);
            if (tokens.empty()) continue;

            string first = tokens.front();
            if (!first.empty() && first.back() == ':') first.pop_back();
            for (string& token : tokens)
            {
                size_t pos = token.find('>');
                if (pos != string::npos && pos > 0 && pos + 1 < token.size())
                {
                    string object_name = token.substr(0, pos);
                    string field_name = token.substr(pos + 1);
                    if (find_symbol(var_name, object_name + "_" + field_name) != -1)
                        token = object_name + "_" + field_name;
                }
            }

            // List-specific instructions are lowered here so their elements remain
            // ordinary Hydronic values for the existing arithmetic/compiler pipeline.
            if (first == "lst_set")
            {
                if (tokens.size() < 4 || tokens.back() != ";")
                    throw runtime_error("P030: Expected 'lst_set list count value...;'");
                int li = find_list(lists, tokens[1]);
                if (li == -1) throw runtime_error("P031: Unknown list: " + tokens[1]);
                unsigned long long count = get_number(tokens[2]);
                if (count > lists[li].length || tokens.size() != count + 4)
                    throw runtime_error("P032: lst_set count does not match arguments or list length");
                for (unsigned i = 0; i < count; ++i)
                {
                    vector<string> lowered = {"set", list_element_name(lists[li].name, i), tokens[3 + i], ";"};
                    compile_instruction(generated_code, lowered, var_name, var_bits,
                        const_name, const_bits, const_value, libs_name, libs_alias, libs, objects);
                }
                for (unsigned i = (unsigned)count; i < lists[li].length; ++i)
                {
                    vector<string> lowered = {"set", list_element_name(lists[li].name, i), "0", ";"};
                    compile_instruction(generated_code, lowered, var_name, var_bits,
                        const_name, const_bits, const_value, libs_name, libs_alias, libs, objects);
                }
                continue;
            }
            if (first == "str_set")
{
                string dst_name, source;

                if (!parse_str_set_line(instruction, dst_name, source))
                    throw runtime_error("P033: Expected 'str_set list[:start-end] source;'");

                string dst_range;
                size_t dst_colon = dst_name.find(':');
                bool destination_slice = dst_colon != string::npos;
                if (destination_slice) { dst_range = dst_name.substr(dst_colon + 1); dst_name = dst_name.substr(0, dst_colon); }
                int dst = find_list(lists, dst_name);
                if (dst == -1) throw runtime_error("P031: Unknown destination list: " + dst_name);

                unsigned dst_begin = 0, dst_count = lists[dst].length;
                if (destination_slice)
                {
                    size_t dash = dst_range.find('-');
                    if (dash == string::npos) throw runtime_error("P035: Expected destination slice start-end");
                    unsigned long long b = get_number(dst_range.substr(0, dash));
                    unsigned long long e = get_number(dst_range.substr(dash + 1));
                    if (b > e || e >= lists[dst].length) throw runtime_error("P036: Destination slice out of range");
                    dst_begin = (unsigned)b; dst_count = (unsigned)(e - b + 1);
                }

                vector<string> source_values;

                if (source.size() >= 2 &&
                    ((source.front() == '"' && source.back() == '"') ||
                    (source.front() == '\'' && source.back() == '\'')))
                {
                    string value = source.substr(1, source.size() - 2);

                    for (unsigned char c : value)
                        source_values.push_back(to_string((unsigned)c));
                }
                else
                {
                    string src_name = source;
                    string src_range;

                    size_t colon = source.find(':');
                    bool source_slice = colon != string::npos;

                    if (source_slice)
                    {
                        src_range = source.substr(colon + 1);
                        src_name = source.substr(0, colon);
                    }

                    int src = find_list(lists, src_name);

                    if (src == -1)
                    {
                        // Если источник не является списком,
                        // считаем весь остаток строки обычным текстом.
                        for (unsigned char c : source)
                            source_values.push_back(to_string((unsigned)c));
                    }
                    else
                    {
                        unsigned src_begin = 0;
                        unsigned src_count = lists[src].length;

                        if (source_slice)
                        {
                            size_t dash = src_range.find('-');

                            if (dash == string::npos)
                                throw runtime_error("P035: Expected source slice start-end");

                            unsigned long long b = get_number(src_range.substr(0, dash));
                            unsigned long long e = get_number(src_range.substr(dash + 1));

                            if (b > e || e >= lists[src].length)
                                throw runtime_error("P036: Source slice out of range");

                            src_begin = (unsigned)b;
                            src_count = (unsigned)(e - b + 1);
                        }

                        for (unsigned i = 0; i < src_count; ++i)
                        {
                            string temp = "_hyd_str_copy_" +
                                to_string(++list_codegen_serial);

                            var_name.push_back(temp);
                            var_bits.push_back((unsigned char)lists[src].bits);
                            var_value.push_back(0);

                            vector<string> load = {
                                "set", temp,
                                list_element_name(src_name, src_begin + i),
                                ";"
                            };

                            compile_instruction(
                                generated_code, load,
                                var_name, var_bits,
                                const_name, const_bits, const_value,
                                libs_name, libs_alias, libs, objects
                            );

                            source_values.push_back(temp);
                        }
                    }
                }

                if (source_values.size() > dst_count)
                    throw runtime_error("P037: str_set source is longer than destination range");
                for (unsigned i = 0; i < dst_count; ++i)
                {
                    string value = i < source_values.size() ? source_values[i] : "0";
                    vector<string> write = {"set", list_element_name(dst_name, dst_begin + i), value, ";"};
                    compile_instruction(generated_code, write, var_name, var_bits, const_name, const_bits, const_value, libs_name, libs_alias, libs, objects);
                }
                continue;
            }

            if (first == "for")
            {
                if (tokens.size() != 5 || tokens.back().empty() || tokens.back().back() != ':')
                    throw runtime_error("P022: Expected 'for name start end sign:'");
                string counter_name = tokens[1];
                if (!is_identifier(counter_name))
                    throw runtime_error("P023: Invalid for-counter name: " + counter_name);
                string start = resolve_counter_name(active_counters, tokens[2]);
                string end = resolve_counter_name(active_counters, tokens[3]);
                auto materialize_list_operand = [&](string operand) -> string {
                    string list_name, index;
                    if (!parse_list_reference(operand, list_name, index)) return operand;
                    int li = find_list(lists, list_name);
                    if (li == -1) throw runtime_error("P003: Unknown list: " + list_name);
                    string temp = "_hyd_for_list_value_" + to_string(++list_codegen_serial);
                    var_name.push_back(temp); var_bits.push_back((unsigned char)lists[li].bits); var_value.push_back(0);
                    emit_list_load(generated_code, lists[li], index, var_name, const_name, var_bits, const_bits, const_value, list_codegen_serial);
                    store_value(generated_code, temp, lists[li].bits);
                    return temp;
                };
                start = materialize_list_operand(start);
                end = materialize_list_operand(end);
                string sign = tokens[4].substr(0, tokens[4].size() - 1);
                if (sign != "0" && sign != "1")
                    throw runtime_error("P024: for direction must be 0 or 1");

                unsigned bits = max(operand_bits(start, var_name, var_bits, const_name, const_bits, const_value),
                                    operand_bits(end, var_name, var_bits, const_name, const_bits, const_value));
                unsigned id = ++next_construction_id;
                string storage = "_hyd_for_counter_" + to_string(id);
                string bound = "_hyd_for_bound_" + to_string(id);
                string loop = "_hyd_for_start_" + to_string(id);
                string finish = "_hyd_for_end_" + to_string(id);

                var_name.push_back(storage);
                var_bits.push_back((unsigned char)bits);
                var_value.push_back(0);
                var_name.push_back(bound);
                var_bits.push_back(32);
                var_value.push_back(0);

                load_value(generated_code, start, var_name, const_name, var_bits, const_bits, const_value);
                store_value(generated_code, storage, bits, "eax");
                load_value(generated_code, end, var_name, const_name, var_bits, const_bits, const_value);
                generated_code << "    mov DWORD PTR [" << bound << "], eax\n";
                generated_code << loop << ":\n";
                load_value(generated_code, storage, var_name, const_name, var_bits, const_bits, const_value);
                generated_code << "    cmp eax, DWORD PTR [" << bound << "]\n";
                generated_code << (sign == "0" ? "    ja " : "    jb ") << finish << "\n";

                ConstructionBlock block;
                block.type = "for";
                block.end_label = finish;
                block.loop_label = loop;
                block.counter_name = counter_name;
                block.counter_storage = storage;
                block.bound_storage = bound;
                block.descending = sign == "1";
                construction_stack.push_back(block);
                active_counters.push_back({counter_name, storage});
                continue;
            }

            if (first == "if" || first == "elseif" || first == "else")
            {
                for (size_t i = 1; i < tokens.size(); ++i)
                {
                    if (tokens[i] == ":") continue;
                    bool has_colon = !tokens[i].empty() && tokens[i].back() == ':';
                    string name = has_colon ? tokens[i].substr(0, tokens[i].size() - 1) : tokens[i];
                    tokens[i] = resolve_counter_name(active_counters, name) + (has_colon ? ":" : "");
                }
                if (tokens.size() >= 2)
                {
                    string condition_token = tokens[1];
                    bool condition_colon = !condition_token.empty() && condition_token.back() == ':';
                    if (condition_colon) condition_token.pop_back();
                    string list_name, index;
                    if (parse_list_reference(condition_token, list_name, index))
                    {
                        int li = find_list(lists, list_name);
                        if (li != -1)
                        {
                            string temp = "_hyd_list_condition_" + to_string(++list_codegen_serial);
                            var_name.push_back(temp); var_bits.push_back((unsigned char)lists[li].bits); var_value.push_back(0);
                            emit_list_load(generated_code, lists[li], index, var_name, const_name, var_bits, const_bits, const_value, list_codegen_serial);
                            store_value(generated_code, temp, lists[li].bits);
                            tokens[1] = temp + (condition_colon ? ":" : "");
                        }
                    }
                }
                compile_construction(generated_code, tokens, var_name, var_bits,
                    const_name, const_bits, const_value,
                    construction_stack, next_construction_id);
            }
            else if (first == "end")
            {
                if (tokens.size() != 2 || tokens[1] != ";")
                    throw runtime_error("P019: Expected 'end;'");
                if (construction_stack.empty())
                    throw runtime_error("P020: 'end' without matching construction");

                ConstructionBlock block = construction_stack.back();
                construction_stack.pop_back();
                if (block.type == "for")
                {
                    load_value(generated_code, block.counter_storage, var_name, const_name,
                        var_bits, const_bits, const_value);
                    generated_code << "    cmp eax, DWORD PTR [" << block.bound_storage << "]\n";
                    generated_code << "    je " << block.end_label << "\n";
                    generated_code << "    " << (block.descending ? "dec " : "inc ")
                                   << storage_type(var_bits[find_symbol(var_name, block.counter_storage)])
                                   << " PTR [" << block.counter_storage << "]\n";
                    generated_code << "    jmp " << block.loop_label << "\n";
                    generated_code << block.end_label << ":\n";
                    if (active_counters.empty() || active_counters.back().storage != block.counter_storage)
                        throw runtime_error("I004: Internal for-counter scope error");
                    active_counters.pop_back();
                }
                else if (block.type == "while")
                {
                    generated_code << "    jmp " << block.loop_label << "\n";
                    generated_code << block.end_label << ":\n";
                }
                else
                {
                    if (!block.has_else) generated_code << block.false_label << ":\n";
                    generated_code << block.end_label << ":\n";
                }
            }
            else if (first == "while")
            {
                if (tokens.size() != 2 || tokens[1].empty() || tokens[1].back() != ':')
                    throw runtime_error("P026: Expected 'while boolvar:'");

                string condition = tokens[1].substr(0, tokens[1].size() - 1);
                condition = resolve_counter_name(active_counters, condition);
                string list_name, index;
                int condition_var = find_symbol(var_name, condition);
                if (parse_list_reference(condition, list_name, index))
                {
                    int li = find_list(lists, list_name);
                    if (li == -1) throw runtime_error("P003: Unknown list: " + list_name);
                    string temp = "_hyd_list_while_" + to_string(++list_codegen_serial);
                    var_name.push_back(temp); var_bits.push_back((unsigned char)lists[li].bits); var_value.push_back(0);
                    condition = temp;
                    // Re-evaluate the list element at the top of every while iteration.
                    condition_var = find_symbol(var_name, condition);
                }
                if (condition_var == -1) throw runtime_error("P003: Unknown variable: " + condition);
                if (var_bits[condition_var] != 1) throw runtime_error("P027: while condition must be a 1-bit variable: " + condition);

                unsigned id = ++next_construction_id;
                ConstructionBlock block;
                block.type = "while";
                block.loop_label = "_hyd_while_start_" + to_string(id);
                block.end_label = "_hyd_while_end_" + to_string(id);

                generated_code << block.loop_label << ":\n";
                string while_list_name, while_index;
                if (parse_list_reference(tokens[1].substr(0, tokens[1].size() - 1), while_list_name, while_index))
                {
                    int li = find_list(lists, while_list_name);
                    emit_list_load(generated_code, lists[li], while_index, var_name, const_name, var_bits, const_bits, const_value, list_codegen_serial);
                    store_value(generated_code, condition, var_bits[condition_var]);
                }
                load_value(generated_code, condition, var_name, const_name,
                    var_bits, const_bits, const_value);
                generated_code << "    test eax, eax\n";
                generated_code << "    jz " << block.end_label << "\n";

                construction_stack.push_back(block);
            }
            else
            {
                vector<string> original_tokens = tokens;
                string op = tokens[0];
                vector<string> args(tokens.begin() + 1, tokens.end());
                if (!args.empty() && args.back() == ";") args.pop_back();
                bool writes_first = (op == "set" || op == "add" || op == "sub" || op == "mul" ||
                    op == "div" || op == "mod" || op == "s_add" || op == "s_sub" ||
                    op == "s_mul" || op == "s_div" || op == "s_mod" || op == "and" ||
                    op == "or" || op == "xor" || op == "not" || op == "bit");
                if (writes_first && !args.empty() && find_active_counter(active_counters, args[0]) != -1)
                    throw runtime_error("P025: for-counter is read-only: " + args[0]);
                if (op == "cmp" && args.size() >= 3 && find_active_counter(active_counters, args[2]) != -1)
                    throw runtime_error("P025: for-counter is read-only: " + args[2]);
                if (op.size() >= 3 && op.front() == '<' && op.back() == '>')
                {
                    int li = vector_find(libs_alias, op.substr(1, op.size() - 2));
                    if (li != -1 && args.size() >= 1)
                    {
                        int fi = find_library_function(libs[li], args[0]);
                        if (fi != -1)
                            for (size_t ai = 0; ai < libs[li].functions[fi].args.size() && ai + 1 < args.size(); ++ai)
                                if (libs[li].functions[fi].args[ai].type == "var" &&
                                    find_active_counter(active_counters, args[ai + 1]) != -1)
                                    throw runtime_error("P025: for-counter is read-only: " + args[ai + 1]);
                    }
                }

                for (size_t i = 1; i < tokens.size(); ++i)
                    if (tokens[i] != ";") tokens[i] = resolve_counter_name(active_counters, tokens[i]);

                vector<pair<string, string>> list_writebacks;
                string operation = tokens[0];
                for (size_t ti = 1; ti + 1 < tokens.size(); ++ti)
                {
                    string list_name, index;
                    if (!parse_list_reference(tokens[ti], list_name, index)) continue;
                    index = resolve_counter_name(active_counters, index);
                    int li = find_list(lists, list_name);
                    if (li == -1) continue;
                    bool writes_first = (operation == "set" || operation == "add" || operation == "sub" ||
                        operation == "mul" || operation == "div" || operation == "mod" ||
                        operation == "s_add" || operation == "s_sub" || operation == "s_mul" ||
                        operation == "s_div" || operation == "s_mod" || operation == "and" ||
                        operation == "or" || operation == "xor" || operation == "not" || operation == "bit");
                    bool cmp_destination = operation == "cmp" && ti == 3;
                    bool destination = (ti == 1 && writes_first) || cmp_destination;
                    bool read_before_write = !(destination && operation == "set");
                    string temp = "_hyd_list_operand_" + to_string(++list_codegen_serial);
                    var_name.push_back(temp); var_bits.push_back((unsigned char)lists[li].bits); var_value.push_back(0);
                    if (read_before_write)
                    {
                        emit_list_load(generated_code, lists[li], index, var_name, const_name,
                            var_bits, const_bits, const_value, list_codegen_serial);
                        store_value(generated_code, temp, lists[li].bits);
                    }
                    tokens[ti] = temp;
                    if (destination) list_writebacks.push_back({list_name, index + "|" + temp});
                }
                compile_instruction(generated_code, tokens, var_name, var_bits,
                    const_name, const_bits, const_value,
                    libs_name, libs_alias, libs, objects);
                for (const auto& wb : list_writebacks)
                {
                    size_t split_at = wb.second.find('|');
                    string index = wb.second.substr(0, split_at);
                    string temp = wb.second.substr(split_at + 1);
                    load_value(generated_code, temp, var_name, const_name, var_bits, const_bits, const_value);
                    int li = find_list(lists, wb.first);
                    emit_list_store(generated_code, lists[li], index, var_name, const_name,
                        var_bits, const_bits, const_value, list_codegen_serial);
                }
            }
        }

        if (!construction_stack.empty())
            throw runtime_error("P021: Missing 'end;' for construction");

        asmp << "\n.data\n";
        for (size_t i = 0; i < var_name.size(); i++)
            asmp << "    " << var_name[i] << " " << storage_type(var_bits[i]) << " 0\n";
        for (const HlibLibrary& library : libs)
            for (const string& data_line : library.data_code)
                asmp << data_line << "\n";

        asmp << "\n.code\nmain PROC\n";
        asmp << generated_code.str();
        asmp << "    invoke ExitProcess, 0\n";
        asmp << "main ENDP\n\ndivision_error:\n    invoke ExitProcess, 1\nlist_index_error:\n    invoke ExitProcess, 2\nEND main\n";
        cout << "Hydronic: generated program.asm\n";
    }
    catch (const exception& e)
    {
        cerr << "Hydronic compile error: " << e.what() << '\n';
        return 1;
    }
    return 0;
}
