//------------------------------------------------------------------------------
//! @file main.cpp
//! @brief slang-blocks -- extract a per-level block-diagram model from an
//!        elaborated SystemVerilog design.
//
// The tool emits facts only: instances, pins, resolved parameters, every net
// endpoint with its direction, and interface connections with their modport.
// It makes no drawing decisions -- no hidden clocks, no bus collapsing, no
// styling.  Those belong to the renderer, which consumes this JSON.
//
// Schema: slang-blocks-model/1  (shared with util/slang_hier_to_dot.py in the
// ibex repo, which can both produce and consume it).
//
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#include <algorithm>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "slang/ast/ASTVisitor.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/CompilationUnitSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/MemberSymbols.h"
#include "slang/ast/symbols/ParameterSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/ast/symbols/VariableSymbols.h"
#include "slang/driver/Driver.h"
#include "slang/syntax/SyntaxNode.h"
#include "slang/text/SourceManager.h"
#include "slang/util/OS.h"
#include "slang/util/VersionInfo.h"

using namespace slang;
using namespace slang::ast;
using namespace slang::driver;

namespace {

constexpr auto SCHEMA = "slang-blocks-model/2";

//------------------------------------------------------------------------------
// Model
//------------------------------------------------------------------------------

struct NodeInfo {
    std::string key, name, type, kind;
    int count = 1;
    std::vector<std::string> params;
    std::vector<std::string> generate;
};

struct EndpointInfo {
    std::string node, port;
    bool drives = false, reads = false, unknown = false;
};

struct NetInfo {
    std::string name;
    uint64_t width = 0;          // 0 when the type has no meaningful width
    std::vector<EndpointInfo> endpoints;
};

/// A connection that is not a net: produced by eliding a block, standing for
/// the path that ran through it.
struct EdgeInfo {
    std::string from, to, via;
    bool bidir = false;
    std::vector<std::string> labels;
};

struct LevelInfo {
    std::string module, instance;
    std::vector<std::string> params;
    std::vector<NodeInfo> nodes;
    std::vector<std::string> nodeOrder;          // keys, in declaration order
    std::map<std::string, size_t> nodeIndex;
    std::vector<std::string> netOrder;           // paths, in discovery order
    std::map<std::string, NetInfo> nets;
    // (from, to, modport) -> (drives, reads); a set keeps duplicates out when
    // a replicated block is visited once per copy.
    std::map<std::tuple<std::string, std::string, std::string>,
             std::pair<bool, bool>>
        ifaces;
    std::vector<EdgeInfo> edges;

    void addNode(const NodeInfo& n) {
        auto it = nodeIndex.find(n.key);
        if (it == nodeIndex.end()) {
            nodeIndex[n.key] = nodes.size();
            nodeOrder.push_back(n.key);
            nodes.push_back(n);
        }
        else {
            nodes[it->second].count = std::max(nodes[it->second].count, n.count);
        }
    }

    void addEndpoint(const std::string& path, const std::string& name,
                     const EndpointInfo& ep, uint64_t width = 0) {
        auto it = nets.find(path);
        if (it == nets.end()) {
            netOrder.push_back(path);
            nets[path] = NetInfo{name, width, {ep}};
        }
        else {
            it->second.endpoints.push_back(ep);
            if (it->second.width == 0)
                it->second.width = width;
        }
    }
};

//------------------------------------------------------------------------------
// Small helpers
//------------------------------------------------------------------------------

const std::regex INDEX_RE(R"(\[[0-9]+\])");

/// Drop array indices, so replicas share one key.
std::string canon(std::string_view path) {
    return std::regex_replace(std::string(path), INDEX_RE, "");
}

/// Path relative to the level instance; nullopt if not inside it.
std::optional<std::string> rel(const std::string& path, const std::string& base) {
    auto p = canon(path), b = canon(base);
    if (p == b)
        return std::string{};
    if (p.size() > b.size() + 1 && p.compare(0, b.size(), b) == 0 && p[b.size()] == '.')
        return p.substr(b.size() + 1);
    return std::nullopt;
}

std::string join(const std::vector<std::string>& parts, char sep) {
    std::string out;
    for (auto& p : parts) {
        if (!out.empty())
            out += sep;
        out += p;
    }
    return out;
}

/// How many copies a generate-block path stands for.
int gpathMultiplicity(const std::vector<std::string>& gpath) {
    static const std::regex mult(R"(\[x([0-9]+)\])");
    int n = 1;
    for (auto& part : gpath) {
        for (auto it = std::sregex_iterator(part.begin(), part.end(), mult);
             it != std::sregex_iterator(); ++it) {
            n *= std::stoi((*it)[1].str());
        }
    }
    return n;
}

/// Readable form of a resolved parameter value.  Mirrors fmt_value() in the
/// Python reference implementation: wide or negative integers as hex over
/// their own bit pattern, string literals as text, one-bit as 1'bN.
std::string formatValue(const ConstantValue& value) {
    if (!value.isInteger())
        return value.toString();

    const SVInt& sv = value.integer();
    if (sv.hasUnknown())
        return value.toString();

    auto width = sv.getBitWidth();
    auto asInt = sv.as<int64_t>();
    if (!asInt.has_value())
        return value.toString();
    int64_t i = *asInt;

    if (width == 1)
        return std::string("1'b") + char('0' + (i & 1));

    if (width >= 16 && width <= 256 && width % 8 == 0 && i > 0) {
        std::string text;
        bool printable = true;
        for (int b = int(width / 8) - 1; b >= 0; b--) {
            auto c = char((uint64_t(i) >> (b * 8)) & 0xFF);
            if (c < 0x20 || c >= 0x7F) {
                printable = false;
                break;
            }
            text += c;
        }
        if (printable)
            return "\"" + text + "\"";
    }

    if (i < 0 || std::abs(i) >= 4096) {
        uint64_t mask = width >= 64 ? ~0ULL : ((1ULL << width) - 1);
        std::ostringstream os;
        os << "0x" << std::uppercase << std::hex << (uint64_t(i) & mask);
        return os.str();
    }
    return std::to_string(i);
}

/// Resolved parameters worth putting on a label.
std::vector<std::string> scalarParams(const InstanceBodySymbol& body, size_t limit) {
    std::vector<std::string> out;
    for (auto p : body.getParameters()) {
        if (out.size() >= limit)
            break;
        if (p->symbol.kind != SymbolKind::Parameter)
            continue;
        auto& param = p->symbol.as<ParameterSymbol>();
        if (param.isLocalParam())
            continue;
        auto text = formatValue(param.getValue());
        if (text.size() > 24 || text.find('\n') != std::string::npos)
            continue;
        out.push_back(std::string(param.name) + "=" + text);
    }
    return out;
}

/// Declared width of a signal, or 0 when the type has none worth reporting.
uint64_t symbolWidth(const ValueSymbol& sym) {
    auto& type = sym.getType();
    if (type.isIntegral())
        return type.getBitWidth();
    auto bits = type.getBitstreamWidth();
    return bits;
}

/// Every value symbol referenced anywhere in an expression.
struct RefCollector : public ASTVisitor<RefCollector, VisitFlags::AllGood> {
    std::vector<const ValueSymbol*> syms;
    void handle(const NamedValueExpression& e) { syms.push_back(&e.symbol); }
    void handle(const HierarchicalValueExpression& e) { syms.push_back(&e.symbol); }
};

std::vector<const ValueSymbol*> refSymbols(const Expression* expr) {
    RefCollector c;
    if (expr)
        expr->visit(c);
    return c.syms;
}

/// (module drives, module reads) for a modport.
std::pair<bool, bool> modportDirections(const ModportSymbol* modport) {
    if (!modport)
        return {true, true};
    bool drives = false, reads = false;
    for (auto& m : modport->members()) {
        if (m.kind != SymbolKind::ModportPort)
            continue;
        switch (m.as<ModportPortSymbol>().direction) {
            case ArgumentDirection::Out: drives = true; break;
            case ArgumentDirection::In: reads = true; break;
            default: drives = reads = true; break;
        }
    }
    if (!drives && !reads)
        return {true, true};
    return {drives, reads};
}

/// (reads, drives) as seen from inside the instantiated child.
std::pair<bool, bool> portDirection(const Symbol& port) {
    if (port.kind == SymbolKind::Port) {
        switch (port.as<PortSymbol>().direction) {
            case ArgumentDirection::Out: return {false, true};
            case ArgumentDirection::In: return {true, false};
            default: return {true, true};
        }
    }
    return {true, true};
}

/// Named port connections of an unknown module, recovered from its syntax.
///
/// slang binds nothing inside an instantiation whose definition it never saw,
/// so the elaborated connection expressions are all invalid.  The syntax tree
/// still has the text.  Positional connections are not recovered.
std::vector<std::pair<std::string, std::vector<std::string>>> blackboxConnections(
    const UninstantiatedDefSymbol& sym) {

    std::vector<std::pair<std::string, std::vector<std::string>>> out;
    auto syntax = sym.getSyntax();
    if (!syntax)
        return out;

    std::string text = syntax->toString();
    text = std::regex_replace(text, std::regex(R"(//[^\n]*)"), " ");
    text = std::regex_replace(text, std::regex(R"(/\*[\s\S]*?\*/)"), " ");

    static const std::regex portRe(R"(\.([A-Za-z_][A-Za-z0-9_]*)\s*\()");
    static const std::regex identRe(R"([A-Za-z_][A-Za-z0-9_]*)");

    for (auto it = std::sregex_iterator(text.begin(), text.end(), portRe);
         it != std::sregex_iterator(); ++it) {
        size_t j = size_t(it->position(0) + it->length(0));
        int depth = 1;
        while (j < text.size() && depth) {
            if (text[j] == '(')
                depth++;
            else if (text[j] == ')')
                depth--;
            j++;
        }
        std::string inner = text.substr(size_t(it->position(0) + it->length(0)),
                                        j - size_t(it->position(0) + it->length(0)) - 1);
        std::vector<std::string> idents;
        for (auto i2 = std::sregex_iterator(inner.begin(), inner.end(), identRe);
             i2 != std::sregex_iterator(); ++i2) {
            idents.push_back(i2->str());
        }
        out.emplace_back((*it)[1].str(), idents);
    }
    return out;
}

//------------------------------------------------------------------------------
// Extraction
//------------------------------------------------------------------------------

class LevelBuilder {
public:
    LevelBuilder(const InstanceSymbol& inst, const std::string& module) :
        instance(inst), base(inst.getHierarchicalPath()) {
        level.module = module;
        level.instance = base;
        level.params = scalarParams(inst.body, 8);
    }

    LevelInfo build() {
        collectPorts();
        walkScope(instance.body, {});
        finishGlue();
        addPortEndpoints();
        return level;
    }

private:
    const InstanceSymbol& instance;
    std::string base;
    LevelInfo level;

    // path -> (node key, drives, reads) for the level's own pins
    std::map<std::string, std::tuple<std::string, bool, bool>> portSyms;
    // (resolved outside path, modport) -> pin key
    std::map<std::pair<std::string, std::string>, std::string> ifacePorts;
    std::set<std::string> glueDriven, glueRead;
    bool haveGlue = false;

    void collectPorts() {
        for (auto portSym : instance.body.getPortList()) {
            if (portSym->kind == SymbolKind::InterfacePort) {
                auto& p = portSym->as<InterfacePortSymbol>();
                std::string key = "port:" + std::string(p.name);
                NodeInfo n;
                n.key = key;
                n.name = std::string(p.name);
                n.type = p.interfaceDef ? std::string(p.interfaceDef->name) : "interface";
                n.kind = "port_iface";
                level.addNode(n);
                portSyms[canon(p.getHierarchicalPath())] = {key, true, true};

                // An interface port is resolved to the interface instance in
                // the *parent*, so children of this level connect to something
                // outside it.  Map those outside paths back onto our own pins,
                // keyed by modport: several pins can be different modports of
                // one interface instance.
                auto conn = p.getConnection();
                if (conn.first) {
                    auto outside = canon(conn.first->getHierarchicalPath());
                    if (!outside.empty()) {
                        std::string mp = conn.second ? std::string(conn.second->name)
                                                     : std::string(p.modport);
                        ifacePorts.emplace(std::make_pair(outside, mp), key);
                        ifacePorts.emplace(std::make_pair(outside, std::string{}), key);
                    }
                }
                continue;
            }

            if (portSym->kind != SymbolKind::Port)
                continue;
            auto& p = portSym->as<PortSymbol>();
            bool isInput = p.direction == ArgumentDirection::In;
            std::string key = "port:" + std::string(p.name);
            NodeInfo n;
            n.key = key;
            n.name = std::string(p.name);
            n.kind = isInput ? "port_in" : "port_out";
            level.addNode(n);
            if (p.internalSymbol) {
                // From inside the level an input pin is a driver.
                portSyms[canon(p.internalSymbol->getHierarchicalPath())] = {key, isInput,
                                                                           !isInput};
            }
        }
    }

    void walkScope(const Scope& scope, std::vector<std::string> gpath) {
        for (auto& member : scope.members()) {
            switch (member.kind) {
                case SymbolKind::UninstantiatedDef:
                    addBlackbox(member.as<UninstantiatedDefSymbol>(), scope, gpath);
                    break;

                case SymbolKind::Instance:
                    addInstance(member.as<InstanceSymbol>(), std::string(member.name), 1,
                                gpath);
                    break;

                case SymbolKind::InstanceArray: {
                    auto& arr = member.as<InstanceArraySymbol>();
                    std::vector<const InstanceSymbol*> els;
                    for (auto e : arr.elements) {
                        if (e->kind == SymbolKind::Instance)
                            els.push_back(&e->as<InstanceSymbol>());
                    }
                    for (auto e : els)
                        addInstance(*e, std::string(arr.name), int(els.size()), gpath);
                    break;
                }

                case SymbolKind::GenerateBlock: {
                    auto& blk = member.as<GenerateBlockSymbol>();
                    if (blk.isUninstantiated)
                        break;
                    auto name = blk.getExternalName();
                    if (name.empty())
                        name = blk.name.empty() ? "genblk" : std::string(blk.name);
                    auto next = gpath;
                    next.push_back(name);
                    walkScope(blk, next);
                    break;
                }

                case SymbolKind::GenerateBlockArray: {
                    auto& arr = member.as<GenerateBlockArraySymbol>();
                    std::vector<const GenerateBlockSymbol*> entries;
                    for (auto e : arr.entries) {
                        if (!e->isUninstantiated)
                            entries.push_back(e);
                    }
                    if (entries.empty())
                        break;
                    auto name = arr.getExternalName();
                    if (name.empty())
                        name = arr.name.empty() ? "genblk" : std::string(arr.name);
                    if (entries.size() > 1)
                        name += "[x" + std::to_string(entries.size()) + "]";
                    auto next = gpath;
                    next.push_back(name);
                    for (auto e : entries)
                        walkScope(*e, next);
                    break;
                }

                case SymbolKind::ContinuousAssign:
                case SymbolKind::ProceduralBlock:
                    collectGlue(member);
                    break;

                default:
                    break;
            }
        }
    }

    void addInstance(const InstanceSymbol& sym, const std::string& name, int count,
                     const std::vector<std::string>& gpath) {
        auto prefix = join(gpath, '/');
        NodeInfo n;
        n.key = (sym.isInterface() ? "iface:" : "inst:") +
                (prefix.empty() ? "" : prefix + "/") + name;
        n.name = name;
        n.type = std::string(sym.getDefinition().name);
        n.kind = sym.isInterface() ? "iface" : "module";
        n.count = count * gpathMultiplicity(gpath);
        n.generate = gpath;
        if (!sym.isInterface())
            n.params = scalarParams(sym.body, 3);
        level.addNode(n);

        if (!sym.isInterface())
            recordConnections(sym, n.key);
    }

    void addBlackbox(const UninstantiatedDefSymbol& sym, const Scope& scope,
                     const std::vector<std::string>& gpath) {
        auto prefix = join(gpath, '/');
        NodeInfo n;
        n.key = "bbox:" + (prefix.empty() ? "" : prefix + "/") + std::string(sym.name);
        n.name = std::string(sym.name);
        n.type = std::string(sym.definitionName);
        n.kind = "blackbox";
        n.generate = gpath;
        level.addNode(n);

        for (auto& [portName, idents] : blackboxConnections(sym)) {
            for (auto& ident : idents) {
                auto found = scope.lookupName(ident);
                if (!found)
                    continue;
                if (found->kind != SymbolKind::Net && found->kind != SymbolKind::Variable &&
                    found->kind != SymbolKind::Port)
                    continue;
                auto path = canon(found->getHierarchicalPath());
                if (!rel(path, base).has_value())
                    continue;
                EndpointInfo ep;
                ep.node = n.key;
                ep.port = portName;
                ep.unknown = true;
                uint64_t width = found->isValue()
                                     ? symbolWidth(found->as<ValueSymbol>())
                                     : 0;
                level.addEndpoint(path, std::string(found->name), ep, width);
            }
        }
    }

    void recordConnections(const InstanceSymbol& sym, const std::string& nodeKey) {
        for (auto conn : sym.getPortConnections()) {
            auto& port = conn->port;

            if (port.kind == SymbolKind::InterfacePort) {
                auto [iface, modport] = conn->getIfaceConn();
                if (!iface)
                    continue;
                auto target = ifaceNodeKey(*iface,
                                           modport ? std::string(modport->name) : std::string{});
                if (target.empty())
                    continue;
                auto [drives, reads] = modportDirections(modport);
                std::string mp = modport ? std::string(modport->name) : std::string{};
                level.ifaces[{nodeKey, target, mp}] = {drives, reads};
                continue;
            }

            auto [reads, drives] = portDirection(port);
            for (auto s : refSymbols(conn->getExpression())) {
                auto path = canon(s->getHierarchicalPath());
                if (!rel(path, base).has_value())
                    continue;
                EndpointInfo ep;
                ep.node = nodeKey;
                ep.port = std::string(port.name);
                ep.drives = drives;
                ep.reads = reads;
                level.addEndpoint(path, std::string(s->name), ep, symbolWidth(*s));
            }
        }
    }

    /// Map a connected interface symbol onto a node of this level.
    std::string ifaceNodeKey(const Symbol& iface, const std::string& modportName) {
        if (iface.kind == SymbolKind::InterfacePort)
            return "port:" + std::string(iface.name);

        auto path = canon(iface.getHierarchicalPath());
        auto r = rel(path, base);
        if (!r.has_value() || r->empty()) {
            // Outside this level: it is what one of our own pins resolved to.
            auto it = ifacePorts.find({path, modportName});
            if (it != ifacePorts.end())
                return it->second;
            it = ifacePorts.find({path, std::string{}});
            return it != ifacePorts.end() ? it->second : std::string{};
        }

        std::string key = "iface:" + *r;
        std::replace(key.begin(), key.end(), '.', '/');
        if (level.nodeIndex.count(key))
            return key;

        // Declared inside a generate block: match on the trailing name.
        auto dot = r->rfind('.');
        auto tail = dot == std::string::npos ? *r : r->substr(dot + 1);
        for (auto& n : level.nodes) {
            if (n.kind == "iface" && n.name == tail)
                return n.key;
        }
        return {};
    }

    void collectGlue(const Symbol& sym) {
        haveGlue = true;
        struct GlueVisitor : public ASTVisitor<GlueVisitor, VisitFlags::AllGood> {
            std::set<std::string>* driven;
            std::set<std::string>* read;
            void handle(const AssignmentExpression& e) {
                for (auto s : refSymbols(&e.left()))
                    driven->insert(canon(s->getHierarchicalPath()));
                for (auto s : refSymbols(&e.right()))
                    read->insert(canon(s->getHierarchicalPath()));
                visitDefault(e);
            }
        };
        GlueVisitor v;
        v.driven = &glueDriven;
        v.read = &glueRead;
        sym.visit(v);
    }

    void finishGlue() {
        if (!haveGlue)
            return;
        std::set<std::string> touched;
        for (auto& p : glueDriven)
            if (rel(p, base).has_value())
                touched.insert(p);
        for (auto& p : glueRead)
            if (rel(p, base).has_value())
                touched.insert(p);
        if (touched.empty())
            return;

        NodeInfo n;
        n.key = "glue";
        n.name = "RTL glue";
        n.type = "assigns / always blocks";
        n.kind = "glue";
        level.addNode(n);

        for (auto& path : touched) {
            EndpointInfo ep;
            ep.node = "glue";
            ep.drives = glueDriven.count(path) > 0;
            ep.reads = glueRead.count(path) > 0;
            auto dot = path.rfind('.');
            level.addEndpoint(path, dot == std::string::npos ? path : path.substr(dot + 1),
                              ep);
        }
    }

    void addPortEndpoints() {
        for (auto& [path, entry] : portSyms) {
            auto& [key, drives, reads] = entry;
            EndpointInfo ep;
            ep.node = key;
            ep.drives = drives;
            ep.reads = reads;
            auto dot = path.rfind('.');
            level.addEndpoint(path, dot == std::string::npos ? path : path.substr(dot + 1),
                              ep);
        }
    }
};

//------------------------------------------------------------------------------
// Filtering: turning the netlist into an abstract schematic
//
// Three operations, applied in file order:
//
//   drop  <what> <regex>              remove matching blocks / nets outright
//   elide <what> <regex>              remove the block but reconnect what ran
//                                     through it, as an indirect edge
//   group <what> <regex> as "<name>"  collapse matches into one block
//
// <what> is one of: inst (instance name), type (module name), net (net name),
// kind (node kind: module, iface, blackbox, glue, port_in, ...).
//
// Elide is the operation that makes this an abstraction rather than a smaller
// netlist: dropping a router in the middle disconnects the core from memory,
// eliding it leaves an edge that says the path exists and names what it went
// through.
//------------------------------------------------------------------------------

enum class RuleOp { Drop, Elide, Group };
enum class RuleWhat { Inst, Type, Net, Kind };

struct Rule {
    RuleOp op;
    RuleWhat what;
    std::regex pattern;
    std::string patternText;
    std::string groupName;
};

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

/// Parses the rule file.  Blank lines and # comments are ignored.
std::vector<Rule> parseFilter(const std::string& text, std::string& error) {
    std::vector<Rule> rules;
    std::istringstream in(text);
    std::string line;
    int lineNo = 0;

    while (std::getline(in, line)) {
        lineNo++;
        auto hash = line.find('#');
        if (hash != std::string::npos)
            line = line.substr(0, hash);
        line = trim(line);
        if (line.empty())
            continue;

        std::istringstream ls(line);
        std::string opText, whatText, pattern;
        ls >> opText >> whatText >> pattern;
        if (pattern.empty()) {
            error = "line " + std::to_string(lineNo) + ": expected '<op> <what> <regex>'";
            return {};
        }

        Rule r;
        if (opText == "drop")
            r.op = RuleOp::Drop;
        else if (opText == "elide")
            r.op = RuleOp::Elide;
        else if (opText == "group")
            r.op = RuleOp::Group;
        else {
            error = "line " + std::to_string(lineNo) + ": unknown operation '" + opText + "'";
            return {};
        }

        if (whatText == "inst")
            r.what = RuleWhat::Inst;
        else if (whatText == "type")
            r.what = RuleWhat::Type;
        else if (whatText == "net")
            r.what = RuleWhat::Net;
        else if (whatText == "kind")
            r.what = RuleWhat::Kind;
        else {
            error = "line " + std::to_string(lineNo) + ": unknown selector '" + whatText + "'";
            return {};
        }

        if (r.what == RuleWhat::Net && r.op == RuleOp::Elide) {
            error = "line " + std::to_string(lineNo) + ": 'elide net' is not meaningful";
            return {};
        }

        r.patternText = pattern;
        try {
            r.pattern = std::regex(pattern, std::regex::ECMAScript);
        }
        catch (const std::regex_error& e) {
            error = "line " + std::to_string(lineNo) + ": bad regex: " + e.what();
            return {};
        }

        if (r.op == RuleOp::Group) {
            std::string as, name;
            ls >> as;
            if (as != "as") {
                error = "line " + std::to_string(lineNo) + ": group needs 'as \"<name>\"'";
                return {};
            }
            std::getline(ls, name);
            name = trim(name);
            if (name.size() >= 2 && name.front() == '"' && name.back() == '"')
                name = name.substr(1, name.size() - 2);
            if (name.empty()) {
                error = "line " + std::to_string(lineNo) + ": group name is empty";
                return {};
            }
            r.groupName = name;
        }
        rules.push_back(std::move(r));
    }
    return rules;
}

bool matchesNode(const Rule& r, const NodeInfo& n) {
    switch (r.what) {
        case RuleWhat::Inst: return std::regex_search(n.name, r.pattern);
        case RuleWhat::Type: return std::regex_search(n.type, r.pattern);
        case RuleWhat::Kind: return std::regex_search(n.kind, r.pattern);
        case RuleWhat::Net: return false;
    }
    return false;
}

/// Rebuild the level with a set of node keys removed, optionally reconnecting
/// through them first.
void removeNodes(LevelInfo& level, const std::set<std::string>& gone, bool reconnect) {
    if (gone.empty())
        return;

    if (reconnect) {
        // For every elided block, join what drove it to what it drove.  Work
        // per block so the "via" label names the thing that was removed.
        for (auto& key : gone) {
            std::vector<std::pair<std::string, std::string>> upstream;   // (node, net)
            std::vector<std::pair<std::string, std::string>> downstream;

            for (auto& path : level.netOrder) {
                auto& net = level.nets[path];
                bool blockReads = false, blockDrives = false;
                for (auto& ep : net.endpoints) {
                    if (ep.node != key)
                        continue;
                    blockReads |= ep.reads || ep.unknown;
                    blockDrives |= ep.drives || ep.unknown;
                }
                if (!blockReads && !blockDrives)
                    continue;
                for (auto& ep : net.endpoints) {
                    if (ep.node == key || gone.count(ep.node))
                        continue;
                    if (blockReads && ep.drives)
                        upstream.emplace_back(ep.node, net.name);
                    if (blockDrives && ep.reads)
                        downstream.emplace_back(ep.node, net.name);
                }
            }
            // Interface connections through the block count too.  The stored
            // (drives, reads) pair is the modport direction as seen by the
            // *instance* side of the entry, so it -- not the order of the
            // tuple -- says which way the path runs.
            for (auto& [k, dr] : level.ifaces) {
                auto& [from, to, mp] = k;
                auto [drives, reads] = dr;
                if (from == key && !gone.count(to)) {
                    if (drives)
                        downstream.emplace_back(to, mp);
                    if (reads)
                        upstream.emplace_back(to, mp);
                }
                else if (to == key && !gone.count(from)) {
                    if (drives)
                        upstream.emplace_back(from, mp);
                    if (reads)
                        downstream.emplace_back(from, mp);
                }
            }

            auto name = key;
            auto colon = name.find(':');
            if (colon != std::string::npos)
                name = name.substr(colon + 1);

            std::map<std::pair<std::string, std::string>, std::set<std::string>> made;
            for (auto& [a, na] : upstream) {
                for (auto& [b, nb] : downstream) {
                    if (a == b)
                        continue;
                    made[{a, b}].insert(na == nb ? na : na + " / " + nb);
                }
            }

            // A path that came out in both directions is one bidirectional
            // link, not two: that is the normal shape for an interface.
            std::set<std::pair<std::string, std::string>> emitted;
            for (auto& [pair, labels] : made) {
                auto [a, b] = pair;
                if (emitted.count({b, a}) || emitted.count({a, b}))
                    continue;
                EdgeInfo e;
                e.from = a;
                e.to = b;
                e.via = name;
                e.bidir = made.count({b, a}) > 0;
                e.labels.assign(labels.begin(), labels.end());
                if (e.bidir) {
                    auto& back = made[{b, a}];
                    for (auto& l : back) {
                        if (std::find(e.labels.begin(), e.labels.end(), l) == e.labels.end())
                            e.labels.push_back(l);
                    }
                }
                emitted.insert({a, b});
                level.edges.push_back(std::move(e));
            }
        }
    }

    // Drop the nodes and every endpoint that referred to them.
    std::vector<NodeInfo> keptNodes;
    for (auto& n : level.nodes) {
        if (!gone.count(n.key))
            keptNodes.push_back(n);
    }
    level.nodes = std::move(keptNodes);
    level.nodeIndex.clear();
    level.nodeOrder.clear();
    for (size_t i = 0; i < level.nodes.size(); i++) {
        level.nodeIndex[level.nodes[i].key] = i;
        level.nodeOrder.push_back(level.nodes[i].key);
    }

    std::vector<std::string> keptNets;
    for (auto& path : level.netOrder) {
        auto& net = level.nets[path];
        std::vector<EndpointInfo> eps;
        for (auto& ep : net.endpoints) {
            if (!gone.count(ep.node))
                eps.push_back(ep);
        }
        if (eps.empty()) {
            level.nets.erase(path);
            continue;
        }
        net.endpoints = std::move(eps);
        keptNets.push_back(path);
    }
    level.netOrder = std::move(keptNets);

    std::map<std::tuple<std::string, std::string, std::string>, std::pair<bool, bool>> ifaces;
    for (auto& [k, v] : level.ifaces) {
        auto& [from, to, mp] = k;
        if (!gone.count(from) && !gone.count(to))
            ifaces[k] = v;
    }
    level.ifaces = std::move(ifaces);

    std::vector<EdgeInfo> edges;
    for (auto& e : level.edges) {
        if (!gone.count(e.from) && !gone.count(e.to))
            edges.push_back(e);
    }
    level.edges = std::move(edges);
}

void applyFilter(LevelInfo& level, const std::vector<Rule>& rules) {
    for (auto& rule : rules) {
        if (rule.what == RuleWhat::Net) {
            if (rule.op == RuleOp::Group)
                continue;  // grouping nets has no meaning here
            std::vector<std::string> kept;
            for (auto& path : level.netOrder) {
                if (std::regex_search(level.nets[path].name, rule.pattern))
                    level.nets.erase(path);
                else
                    kept.push_back(path);
            }
            level.netOrder = std::move(kept);
            continue;
        }

        std::set<std::string> matched;
        for (auto& n : level.nodes) {
            if (matchesNode(rule, n))
                matched.insert(n.key);
        }
        if (matched.empty())
            continue;

        switch (rule.op) {
            case RuleOp::Drop:
                removeNodes(level, matched, /* reconnect */ false);
                break;
            case RuleOp::Elide:
                removeNodes(level, matched, /* reconnect */ true);
                break;
            case RuleOp::Group: {
                // One box stands in for all the matches; every endpoint and
                // interface connection is rewritten onto it.
                std::string key = "group:" + rule.groupName;
                NodeInfo g;
                g.key = key;
                g.name = rule.groupName;
                g.kind = "group";
                g.count = int(matched.size());
                std::set<std::string> types;
                for (auto& n : level.nodes) {
                    if (matched.count(n.key) && !n.type.empty())
                        types.insert(n.type);
                }
                g.type = types.size() == 1 ? *types.begin()
                                           : std::to_string(types.size()) + " module types";

                for (auto& path : level.netOrder) {
                    auto& net = level.nets[path];
                    std::vector<EndpointInfo> eps;
                    std::set<std::tuple<std::string, bool, bool, bool>> seen;
                    for (auto& ep : net.endpoints) {
                        if (matched.count(ep.node)) {
                            ep.node = key;
                            ep.port.clear();
                        }
                        if (seen.insert({ep.node, ep.drives, ep.reads, ep.unknown}).second)
                            eps.push_back(ep);
                    }
                    net.endpoints = std::move(eps);
                }

                std::map<std::tuple<std::string, std::string, std::string>,
                         std::pair<bool, bool>>
                    ifaces;
                for (auto& [k, v] : level.ifaces) {
                    auto [from, to, mp] = k;
                    if (matched.count(from))
                        from = key;
                    if (matched.count(to))
                        to = key;
                    if (from == to)
                        continue;
                    auto it = ifaces.find({from, to, mp});
                    if (it == ifaces.end())
                        ifaces[{from, to, mp}] = v;
                    else
                        it->second = {it->second.first || v.first,
                                      it->second.second || v.second};
                }
                level.ifaces = std::move(ifaces);

                for (auto& e : level.edges) {
                    if (matched.count(e.from))
                        e.from = key;
                    if (matched.count(e.to))
                        e.to = key;
                }

                // Replace the first match in place, so the group keeps a
                // sensible position in declaration order.
                std::vector<NodeInfo> nodes;
                bool placed = false;
                for (auto& n : level.nodes) {
                    if (matched.count(n.key)) {
                        if (!placed) {
                            nodes.push_back(g);
                            placed = true;
                        }
                        continue;
                    }
                    nodes.push_back(n);
                }
                level.nodes = std::move(nodes);
                level.nodeIndex.clear();
                level.nodeOrder.clear();
                for (size_t i = 0; i < level.nodes.size(); i++) {
                    level.nodeIndex[level.nodes[i].key] = i;
                    level.nodeOrder.push_back(level.nodes[i].key);
                }
                break;
            }
        }
    }

    // A net with nothing left on it is not worth carrying.
    std::vector<std::string> kept;
    for (auto& path : level.netOrder) {
        if (level.nets[path].endpoints.empty())
            level.nets.erase(path);
        else
            kept.push_back(path);
    }
    level.netOrder = std::move(kept);
}

//------------------------------------------------------------------------------
// JSON output (hand rolled: no dependency beyond slang itself)
//------------------------------------------------------------------------------

std::string jsonEscape(const std::string& in) {
    std::string out;
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else {
                    out += c;
                }
        }
    }
    return out;
}

std::string jsonString(const std::string& s) {
    return "\"" + jsonEscape(s) + "\"";
}

std::string jsonStringArray(const std::vector<std::string>& v) {
    std::string out = "[";
    for (size_t i = 0; i < v.size(); i++) {
        if (i)
            out += ", ";
        out += jsonString(v[i]);
    }
    return out + "]";
}

std::string emitModel(const std::vector<LevelInfo>& levels) {
    std::ostringstream os;
    os << "{\n \"levels\": [\n";
    for (size_t li = 0; li < levels.size(); li++) {
        auto& lvl = levels[li];
        os << "  {\n";
        os << "   \"instance\": " << jsonString(lvl.instance) << ",\n";

        os << "   \"interfaces\": [\n";
        bool first = true;
        for (auto& [key, dr] : lvl.ifaces) {
            auto& [from, to, mp] = key;
            if (!first)
                os << ",\n";
            first = false;
            os << "    {\"drives\": " << (dr.first ? "true" : "false")
               << ", \"from\": " << jsonString(from) << ", \"modport\": " << jsonString(mp)
               << ", \"reads\": " << (dr.second ? "true" : "false")
               << ", \"to\": " << jsonString(to) << "}";
        }
        os << (first ? "" : "\n") << "   ],\n";

        os << "   \"edges\": [\n";
        for (size_t e = 0; e < lvl.edges.size(); e++) {
            auto& edge = lvl.edges[e];
            os << "    {\"bidir\": " << (edge.bidir ? "true" : "false")
               << ", \"from\": " << jsonString(edge.from)
               << ", \"labels\": " << jsonStringArray(edge.labels)
               << ", \"to\": " << jsonString(edge.to)
               << ", \"via\": " << jsonString(edge.via) << "}"
               << (e + 1 < lvl.edges.size() ? "," : "") << "\n";
        }
        os << "   ],\n";

        os << "   \"module\": " << jsonString(lvl.module) << ",\n";

        os << "   \"nets\": [\n";
        first = true;
        for (auto& path : lvl.netOrder) {
            auto& net = lvl.nets.at(path);
            if (!first)
                os << ",\n";
            first = false;
            os << "    {\n     \"endpoints\": [\n";
            for (size_t e = 0; e < net.endpoints.size(); e++) {
                auto& ep = net.endpoints[e];
                os << "      {\"drives\": " << (ep.drives ? "true" : "false")
                   << ", \"node\": " << jsonString(ep.node)
                   << ", \"port\": " << jsonString(ep.port)
                   << ", \"reads\": " << (ep.reads ? "true" : "false")
                   << ", \"unknown\": " << (ep.unknown ? "true" : "false") << "}"
                   << (e + 1 < net.endpoints.size() ? "," : "") << "\n";
            }
            os << "     ],\n     \"name\": " << jsonString(net.name)
               << ",\n     \"path\": " << jsonString(path)
               << ",\n     \"width\": " << net.width << "\n    }";
        }
        os << (first ? "" : "\n") << "   ],\n";

        os << "   \"nodes\": [\n";
        for (size_t n = 0; n < lvl.nodes.size(); n++) {
            auto& node = lvl.nodes[n];
            os << "    {\"count\": " << node.count
               << ", \"generate\": " << jsonStringArray(node.generate)
               << ", \"key\": " << jsonString(node.key)
               << ", \"kind\": " << jsonString(node.kind)
               << ", \"name\": " << jsonString(node.name)
               << ", \"params\": " << jsonStringArray(node.params)
               << ", \"type\": " << jsonString(node.type) << "}"
               << (n + 1 < lvl.nodes.size() ? "," : "") << "\n";
        }
        os << "   ],\n";

        os << "   \"params\": " << jsonStringArray(lvl.params) << "\n";
        os << "  }" << (li + 1 < levels.size() ? "," : "") << "\n";
    }
    os << " ],\n \"schema\": " << jsonString(SCHEMA) << "\n}\n";
    return os.str();
}

//------------------------------------------------------------------------------
// Finding the requested levels
//------------------------------------------------------------------------------

void collectChildInstances(const Scope& scope,
                           std::vector<const InstanceSymbol*>& out) {
    for (auto& member : scope.members()) {
        switch (member.kind) {
            case SymbolKind::Instance: {
                auto& inst = member.as<InstanceSymbol>();
                if (!inst.isInterface())
                    out.push_back(&inst);
                break;
            }
            case SymbolKind::InstanceArray:
                for (auto e : member.as<InstanceArraySymbol>().elements) {
                    if (e->kind == SymbolKind::Instance &&
                        !e->as<InstanceSymbol>().isInterface())
                        out.push_back(&e->as<InstanceSymbol>());
                }
                break;
            case SymbolKind::GenerateBlock: {
                auto& blk = member.as<GenerateBlockSymbol>();
                if (!blk.isUninstantiated)
                    collectChildInstances(blk, out);
                break;
            }
            case SymbolKind::GenerateBlockArray:
                for (auto e : member.as<GenerateBlockArraySymbol>().entries) {
                    if (!e->isUninstantiated)
                        collectChildInstances(*e, out);
                }
                break;
            default:
                break;
        }
    }
}

/// Breadth-first, so the shallowest instance of each module wins.
std::map<std::string, const InstanceSymbol*> findLevels(
    const RootSymbol& root, const std::set<std::string>& wanted) {

    std::map<std::string, const InstanceSymbol*> found;
    std::set<std::string> seen;
    std::vector<const InstanceSymbol*> queue(root.topInstances.begin(),
                                             root.topInstances.end());

    while (!queue.empty() && found.size() < wanted.size()) {
        std::vector<const InstanceSymbol*> next;
        for (auto inst : queue) {
            std::string name(inst->getDefinition().name);
            if (wanted.count(name) && !found.count(name))
                found[name] = inst;

            auto path = canon(inst->getHierarchicalPath());
            if (!seen.insert(path).second)
                continue;
            collectChildInstances(inst->body, next);
        }
        queue = std::move(next);
    }
    return found;
}

void collectModuleNames(const Scope& scope, std::set<std::string>& names,
                        std::set<std::string>& seen) {
    std::vector<const InstanceSymbol*> children;
    collectChildInstances(scope, children);
    for (auto inst : children) {
        names.insert(std::string(inst->getDefinition().name));
        auto path = canon(inst->getHierarchicalPath());
        if (seen.insert(path).second)
            collectModuleNames(inst->body, names, seen);
    }
}

} // namespace

//------------------------------------------------------------------------------

int main(int argc, char** argv) {
    Driver driver;
    driver.addStandardArgs();

    std::optional<bool> showHelp;
    std::optional<bool> showVersion;
    std::optional<bool> listModules;
    std::optional<std::string> output;
    std::optional<std::string> filterFile;
    std::vector<std::string> levels;

    driver.cmdLine.add("-h,--help", showHelp, "Display available options");
    driver.cmdLine.add("--version", showVersion, "Display version information and exit");
    driver.cmdLine.add("--level", levels,
                       "Module to extract (repeatable); default: the top", "<module>");
    driver.cmdLine.add("-o,--output", output, "Write the model here instead of stdout",
                       "<file>");
    driver.cmdLine.add("--filter", filterFile,
                       "Rule file selecting what the diagram shows "
                       "(drop / elide / group)",
                       "<file>");
    driver.cmdLine.add("--list-modules", listModules,
                       "List module names in the elaborated hierarchy and exit");

    if (!driver.parseCommandLine(argc, argv))
        return 1;

    if (showHelp == true) {
        OS::print(driver.cmdLine.getHelpText(
            "slang-blocks: block-diagram model extractor"));
        return 0;
    }
    if (showVersion == true) {
        OS::print(fmt::format("slang-blocks (slang {})\n",
                              VersionInfo::getVersionString()));
        return 0;
    }
    if (!driver.processOptions())
        return 2;

    bool ok = driver.parseAllSources();
    auto compilation = driver.createCompilation();
    auto& root = compilation->getRoot();

    if (root.topInstances.empty()) {
        OS::printE("error: no top instance was elaborated\n");
        return 3;
    }

    if (listModules == true) {
        std::set<std::string> names, seen;
        for (auto inst : root.topInstances) {
            names.insert(std::string(inst->getDefinition().name));
            collectModuleNames(inst->body, names, seen);
        }
        for (auto& n : names)
            OS::print(n + "\n");
        return 0;
    }

    std::vector<std::string> wanted = levels;
    if (wanted.empty())
        wanted.push_back(std::string(root.topInstances[0]->getDefinition().name));

    auto found = findLevels(root, std::set<std::string>(wanted.begin(), wanted.end()));

    std::vector<Rule> rules;
    if (filterFile.has_value()) {
        FILE* f = fopen(filterFile->c_str(), "rb");
        if (!f) {
            OS::printE("error: cannot read " + *filterFile + "\n");
            return 4;
        }
        std::string text;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            text.append(buf, n);
        fclose(f);

        std::string error;
        rules = parseFilter(text, error);
        if (!error.empty()) {
            OS::printE("error: " + *filterFile + ": " + error + "\n");
            return 4;
        }
    }

    std::vector<LevelInfo> out;
    for (auto& module : wanted) {
        auto it = found.find(module);
        if (it == found.end()) {
            OS::printE("warning: no instance of '" + module +
                       "' in the elaborated hierarchy\n");
            continue;
        }
        auto level = LevelBuilder(*it->second, module).build();
        if (!rules.empty())
            applyFilter(level, rules);
        out.push_back(std::move(level));
    }

    auto json = emitModel(out);
    if (output.has_value()) {
        FILE* f = fopen(output->c_str(), "wb");
        if (!f) {
            OS::printE("error: cannot write " + *output + "\n");
            return 4;
        }
        fwrite(json.data(), 1, json.size(), f);
        fclose(f);
        OS::print(fmt::format("model: {} level(s) -> {}\n", out.size(), *output));
    }
    else {
        OS::print(json);
    }

    ok &= driver.reportDiagnostics(/* quiet */ true);
    return ok ? 0 : 5;
}
