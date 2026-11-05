// Per-language tables and feature flags. buildLanguage() runs exactly once per
// language, on the first languageFor() that resolves to it (see Highlighter.cpp).
#include "code/LangInternal.h"

namespace folia::code {

namespace {

void kw(Language &l, const char *w) { l.kw.add(w, Token::Keyword); }
void ty(Language &l, const char *w) { l.kw.add(w, Token::Type); }
void bi(Language &l, const char *w) { l.kw.add(w, Token::Builtin); }
void co(Language &l, const char *w) { l.kw.add(w, Token::Constant); }

// ---------------------------------------------------------------- C family
void setupC(Language &l)
{
    Features &f = l.f;
    f.blockC = f.lineSlash = f.preproc = f.sq = f.strCont = f.lineCommentCont = f.capConst = f.cppAttr = true;
    f.prefixes = "LuU8";
    f.maxPrefix = 2;
    kw(l, "auto break case const continue default do else enum:T extern for goto if inline register restrict "
          "return sizeof static struct:T switch typedef union:T volatile while _Alignas _Alignof _Atomic _Generic "
          "_Noreturn _Static_assert _Thread_local alignas alignof constexpr static_assert thread_local typeof "
          "typeof_unqual asm");
    ty(l, "int char short long float double void signed unsigned _Bool bool _Complex _Imaginary size_t ssize_t "
          "ptrdiff_t intptr_t uintptr_t intmax_t uintmax_t int8_t int16_t int32_t int64_t uint8_t uint16_t "
          "uint32_t uint64_t wchar_t char16_t char32_t FILE va_list off_t time_t clock_t pid_t");
    bi(l, "printf fprintf sprintf snprintf vprintf scanf sscanf fscanf malloc calloc realloc free memcpy memmove "
          "memset memcmp strlen strcpy strncpy strcmp strncmp strcat strncat strchr strrchr strstr strtok fopen "
          "fclose fread fwrite fgets fputs fseek ftell fflush puts putchar getchar getc putc exit abort atexit "
          "atoi atol atof strtol strtoul strtod qsort bsearch rand srand time clock assert perror");
    co(l, "NULL true false EOF stdin stdout stderr EXIT_SUCCESS EXIT_FAILURE nullptr");
}

void setupCpp(Language &l)
{
    Features &f = l.f;
    f.blockC = f.lineSlash = f.preproc = f.sq = f.strCont = f.lineCommentCont = f.capConst = true;
    f.cppRaw = f.numApos = f.capType = f.cppAttr = true;
    f.prefixes = "LuUR8";
    f.maxPrefix = 3;
    kw(l, "alignas alignof and and_eq asm auto bitand bitor break case catch class:T compl concept:T const "
          "consteval constexpr constinit const_cast continue co_await co_return co_yield decltype default delete do "
          "dynamic_cast else enum:T explicit export extern final for friend goto if inline mutable namespace:T new:T "
          "noexcept not not_eq operator or or_eq override private protected public register reinterpret_cast "
          "requires return sizeof static static_assert static_cast struct:T switch template this thread_local "
          "throw try typedef typeid typename:T union:T using virtual volatile while xor xor_eq");
    ty(l, "bool char char8_t char16_t char32_t double float int long short signed unsigned void wchar_t size_t "
          "ssize_t ptrdiff_t nullptr_t intptr_t uintptr_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t "
          "uint64_t string string_view wstring vector array map multimap set multiset unordered_map unordered_set "
          "list deque queue stack priority_queue pair tuple optional variant any bitset unique_ptr shared_ptr "
          "weak_ptr function span ostream istream iostream ifstream ofstream stringstream ostringstream "
          "istringstream mutex thread atomic");
    bi(l, "std cout cin cerr clog endl printf fprintf sprintf snprintf scanf malloc calloc realloc free memcpy "
          "memmove memset strlen move forward swap make_unique make_shared make_pair make_tuple sort abs assert "
          "exit abort");
    co(l, "true false nullptr NULL");
}

void setupCSharp(Language &l)
{
    Features &f = l.f;
    f.blockC = f.lineSlash = f.preproc = f.sq = f.triple = f.csStr = f.capType = f.capConst = true;
    kw(l, "abstract as base break case catch checked class:T const continue default delegate do else enum:T event "
          "explicit extern finally fixed for foreach goto if implicit in interface:T internal is lock namespace:T "
          "new:T operator out override params private protected public readonly ref return sealed sizeof "
          "stackalloc static struct:T switch this throw try typeof unchecked unsafe using virtual volatile while "
          "async~ await~ yield~ record~ partial~ required~ where~ from~ select~ orderby~ let~ join~ group~ into~ "
          "ascending~ descending~ nameof with~ init~ when~ global~ unmanaged~ managed~");
    ty(l, "bool byte char decimal double float int long object sbyte short string uint ulong ushort void var dynamic "
          "nint nuint");
    bi(l, "Console Math Convert Environment Debug GC");
    co(l, "true false null");
}

void setupJava(Language &l)
{
    Features &f = l.f;
    f.blockC = f.lineSlash = f.sq = f.triple = f.atAttr = f.capType = f.capConst = true;
    kw(l, "abstract assert break case catch class:T const continue default do else enum:T extends:T final finally for "
          "goto if implements:T import instanceof interface:T native new:T package private protected public return "
          "static strictfp super switch synchronized this throw throws transient try volatile while yield "
          "record~ sealed~ permits~");
    ty(l, "boolean byte char double float int long short void var");
    bi(l, "System Math Objects Arrays Collections");
    co(l, "true false null");
}

void setupGo(Language &l)
{
    Features &f = l.f;
    f.blockC = f.lineSlash = f.sq = f.backtickRaw = f.capType = true;
    kw(l, "break case chan const continue default defer else fallthrough for func:F go goto if import interface map "
          "package range return select struct switch type:T var");
    ty(l, "bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune string uint uint8 "
          "uint16 uint32 uint64 uintptr any comparable");
    bi(l, "append cap clear close complex copy delete imag len make max min new panic print println real recover");
    co(l, "true false iota nil");
}

void setupRust(Language &l)
{
    Features &f = l.f;
    f.blockC = f.nest = f.lineSlash = f.dqMulti = f.rustRaw = f.rustAttr = f.rustLifetime = f.rustMacro = true;
    f.capType = f.capConst = true;
    f.prefixes = "bc";
    f.maxPrefix = 1;
    kw(l, "as async await break const continue crate dyn else enum:T extern fn:F for if impl:T in let loop macro_rules "
          "match mod move mut pub ref return self static struct:T super trait:T type:T unsafe use where while "
          "abstract become box do final macro override priv try typeof unsized virtual yield union~");
    ty(l, "bool char f32 f64 i8 i16 i32 i64 i128 isize str u8 u16 u32 u64 u128 usize Self String Vec Option Result "
          "Box Rc Arc");
    bi(l, "Some Ok Err Default Clone Copy Send Sync Sized Drop Iterator IntoIterator From Into ToString");
    co(l, "true false None");
}

void setupSwift(Language &l)
{
    Features &f = l.f;
    f.blockC = f.nest = f.lineSlash = f.triple = f.swiftHash = f.atAttr = f.dollarVar = f.capType = f.capConst = true;
    f.btIdent = true;
    f.btTok = Token::Plain;
    kw(l, "associatedtype class:T deinit enum:T extension:T fileprivate func:F import init inout internal let open "
          "operator private precedencegroup protocol:T public rethrows static struct:T subscript typealias:T var break "
          "case catch continue default defer do else fallthrough for guard if in repeat return throw switch where "
          "while as await is super self throws try actor~ async~ some~ any~ convenience dynamic final lazy mutating "
          "nonmutating optional override required unowned weak willSet didSet indirect nonisolated isolated");
    ty(l, "Int Int8 Int16 Int32 Int64 UInt UInt8 UInt16 UInt32 UInt64 Float Double Bool String Character Array "
          "Dictionary Set Optional Any AnyObject Void Never Self Result Error");
    bi(l, "print debugPrint fatalError precondition assert assertionFailure preconditionFailure min max abs zip "
          "stride readLine");
    co(l, "true false nil");
}

void setupLua(Language &l)
{
    Features &f = l.f;
    f.lineDash = f.luaLong = f.sq = f.strCont = true;
    kw(l, "and break do else elseif end for function:F goto if in local not or repeat return then until while");
    bi(l, "assert collectgarbage dofile error getmetatable ipairs load loadfile next pairs pcall print rawequal "
          "rawget rawlen rawset require select setmetatable tonumber tostring type xpcall _G _ENV _VERSION string "
          "table math io os coroutine debug utf8 package self");
    co(l, "nil true false");
}

void setupPython(Language &l)
{
    Features &f = l.f;
    f.lineHash = f.sq = f.triple = f.tripleSq = f.strCont = f.pyDecor = f.capType = f.capConst = true;
    f.prefixes = "rRbBfFuU";
    f.maxPrefix = 2;
    kw(l, "and as assert async await break class:T continue def:F del elif else except finally for from global if import "
          "in is lambda nonlocal not or pass raise return try while with yield match? case?");
    ty(l, "int str float bool list dict set tuple bytes bytearray object complex frozenset type");
    bi(l, "abs all any bin callable chr dir divmod enumerate eval exec filter format getattr globals hasattr hash "
          "help hex id input isinstance issubclass iter len locals map max min next oct open ord pow print property "
          "range repr reversed round setattr slice sorted staticmethod sum super vars zip self cls classmethod "
          "__name__ __init__ __main__");
    co(l, "True False None __debug__ Ellipsis NotImplemented");
}

void setupJs(Language &l, bool ts)
{
    Features &f = l.f;
    f.blockC = f.lineSlash = f.sq = f.templ = f.regex = f.strCont = f.atAttr = f.dollarIdent = f.hashIdent = true;
    f.capType = f.capConst = true;
    kw(l, "async await break case catch class:T const continue debugger default delete do else export extends:T "
          "finally for function:F if import in instanceof let new:T return static super switch this throw try typeof "
          "var void while with yield of~ from~ as~");
    if (ts) {
        kw(l, "abstract~ asserts~ declare~ enum:T implements:T infer~ interface:T is~ keyof~ module~ namespace:T~ "
              "override~ private protected public readonly~ satisfies~ type:T~ unique~ out~ accessor~");
        ty(l, "any bigint boolean never number object string symbol unknown");
    }
    bi(l, "console window document globalThis Math JSON Object Array String Number Boolean Symbol Promise Map Set "
          "WeakMap WeakSet Date RegExp Error TypeError RangeError parseInt parseFloat isNaN isFinite require module "
          "exports process Buffer setTimeout setInterval clearTimeout clearInterval fetch alert BigInt Intl Reflect "
          "Proxy arguments");
    co(l, "true false null undefined NaN Infinity");
}

void setupSql(Language &l)
{
    Features &f = l.f;
    f.blockC = f.lineDash = f.sqlStr = f.sqMulti = f.dollarQuote = f.btIdent = true;
    f.sq = true;
    f.btTok = Token::Variable;
    kw(l, "select from where insert into values update set delete create alter drop table:T index view database "
          "schema join inner left right full outer cross natural on using group by order having limit offset union "
          "all distinct as and or not in is like ilike between exists case when then else end primary key foreign "
          "references unique default check constraint cascade begin commit rollback transaction with recursive over "
          "partition window asc desc fetch only returning truncate grant revoke explain analyze vacuum pragma if "
          "replace temporary temp trigger function procedure return declare cursor add column rename to modify "
          "engine auto_increment autoincrement collate escape intersect except any some lateral nulls returns");
    ty(l, "int integer bigint smallint tinyint mediumint decimal numeric float double real boolean bool char varchar "
          "nvarchar nchar text tinytext mediumtext longtext blob bytea date time timestamp timestamptz datetime "
          "interval year serial bigserial smallserial uuid json jsonb xml money bit binary varbinary enum array");
    bi(l, "count sum avg min max coalesce ifnull isnull nullif cast convert concat concat_ws length char_length lower "
          "upper substr substring trim ltrim rtrim round ceil ceiling floor abs mod power sqrt now current_date "
          "current_time current_timestamp date_trunc date_part extract to_char to_date to_timestamp strftime random "
          "rand row_number rank dense_rank lag lead first_value last_value ntile string_agg group_concat array_agg "
          "json_agg json_extract generate_series greatest least position instr nvl iif");
    co(l, "null true false unknown");
}

void setupBash(Language &l)
{
    kw(l, "if then else elif fi case esac for select while until do done in function:F time coproc [[ ]] { } !");
    bi(l, "echo printf read cd pwd export unset local declare typeset readonly return exit shift set test eval exec "
          "trap wait kill jobs fg bg alias unalias source type command builtin let getopts umask ulimit history "
          "pushd popd dirs true false : . [ ] hash enable help mapfile readarray shopt disown bind compgen complete");
}

} // namespace

Language *buildLanguage(Lang id)
{
    Language *l = new Language;
    switch (id) {
    case Lang::C: setupC(*l); l->lex = lexCLike; break;
    case Lang::Cpp: setupCpp(*l); l->lex = lexCLike; break;
    case Lang::CSharp: setupCSharp(*l); l->lex = lexCLike; break;
    case Lang::Java: setupJava(*l); l->lex = lexCLike; break;
    case Lang::Go: setupGo(*l); l->lex = lexCLike; break;
    case Lang::Rust: setupRust(*l); l->lex = lexCLike; break;
    case Lang::Swift: setupSwift(*l); l->lex = lexCLike; break;
    case Lang::Lua: setupLua(*l); l->lex = lexCLike; break;
    case Lang::Python: setupPython(*l); l->lex = lexCLike; break;
    case Lang::JavaScript: setupJs(*l, false); l->lex = lexCLike; break;
    case Lang::TypeScript: setupJs(*l, true); l->lex = lexCLike; break;
    case Lang::Sql: l->kw = KwTable(true); setupSql(*l); l->lex = lexCLike; break;
    case Lang::Bash: setupBash(*l); l->lex = lexBash; break;
    case Lang::Css: l->lex = lexCss; break;
    case Lang::Html: l->lex = lexHtml; break;
    case Lang::Json: l->lex = lexJson; break;
    case Lang::Yaml: l->lex = lexYaml; break;
    case Lang::Toml: l->lex = lexToml; break;
    case Lang::Diff: l->lex = lexDiff; break;
    case Lang::Count_: break;
    }
    l->kw.finish();
    return l;
}

} // namespace folia::code
