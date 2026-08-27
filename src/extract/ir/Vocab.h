// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// The closed vocabularies, as enums with their published spellings.
//
// Every value here is interface: it appears in a schema CHECK clause, in
// doc/designdb-schema.md, and in verify-designdb.py's DOMAINS table, which
// --domain-coverage holds the fixture corpus to. Template rows store the
// enum; the word appears where a row becomes SQL.
//
// Domains the schema publishes as OPEN stay strings and are absent here:
// stmt.construct takes a system task's or built-in method's own name,
// net.decl_kind a user-defined nettype's.
//
// slang/util/Util.h is the only dependency, for SLANG_UNREACHABLE; this
// header stays clear of the AST vocabulary so Template.h can hold rows
// without reaching the compilation.

#pragma once

#include <cstdint>

#include "slang/util/Util.h"

namespace designdb::detail {

enum class DepKind : uint8_t { Data, Control, Primitive, Procedure, Alias };
inline const char* word(DepKind k) {
    switch (k) {
        case DepKind::Data:      return "data";
        case DepKind::Control:   return "control";
        case DepKind::Primitive: return "primitive";
        case DepKind::Procedure: return "procedure";
        case DepKind::Alias:     return "alias";
    }
    SLANG_UNREACHABLE;
}

enum class Access : uint8_t { Read, Write, Connect };
inline const char* word(Access a) {
    switch (a) {
        case Access::Read:    return "read";
        case Access::Write:   return "write";
        case Access::Connect: return "connect";
    }
    SLANG_UNREACHABLE;
}

/// What a non-operand read of a STATEMENT is. A branch condition is not
/// among them: it belongs to its level (`branch_ref`), which is where it is
/// written and where it is evaluated once for every statement under it.
enum class RefRole : uint8_t {
    Assertion, Wait, Event, CallArgument, SystemTask
};
inline const char* word(RefRole r) {
    switch (r) {
        case RefRole::Assertion:    return "assertion";
        case RefRole::Wait:         return "wait";
        case RefRole::Event:        return "event";
        case RefRole::CallArgument: return "call_argument";
        case RefRole::SystemTask:   return "system_task";
    }
    SLANG_UNREACHABLE;
}

/// One level of the control context a statement sits under.
///
/// `Case` is the branch POINT -- it carries the selector's reads and the
/// case semantics -- and its arms are its children, because a selector read
/// belongs to the case and a label read belongs to one item, and a single
/// level per item cannot tell the two apart. An `If` is point and arm at
/// once: its two arms share one condition, so every read of the level IS
/// the condition and `sense` says which arm this is.
enum class BranchKind : uint8_t { If, Case, CaseItem, CaseDefault, Loop };
inline const char* word(BranchKind k) {
    switch (k) {
        case BranchKind::If:          return "if";
        case BranchKind::Case:        return "case";
        case BranchKind::CaseItem:    return "case_item";
        case BranchKind::CaseDefault: return "case_default";
        case BranchKind::Loop:        return "loop";
    }
    SLANG_UNREACHABLE;
}

/// None spells the NULL of a branch that is not an `if`.
enum class BranchSense : uint8_t { None, Then, Else };
inline const char* word(BranchSense s) {
    switch (s) {
        case BranchSense::None: return "";
        case BranchSense::Then: return "then";
        case BranchSense::Else: return "else";
    }
    SLANG_UNREACHABLE;
}

/// The matching semantics of a case point, in the LRM's own spelling.
/// `Matches` covers `case ... matches` whatever wildcard word it carries:
/// what a consumer must branch on there is that the items are patterns
/// rather than values. None spells the NULL of a branch that is not one.
enum class CaseKind : uint8_t { None, Case, Casez, Casex, Inside, Matches };
inline const char* word(CaseKind k) {
    switch (k) {
        case CaseKind::None:    return "";
        case CaseKind::Case:    return "case";
        case CaseKind::Casez:   return "casez";
        case CaseKind::Casex:   return "casex";
        case CaseKind::Inside:  return "inside";
        case CaseKind::Matches: return "matches";
    }
    SLANG_UNREACHABLE;
}

/// None spells the NULL of a branch carrying no unique/priority qualifier.
enum class CheckKind : uint8_t { None, Unique, Unique0, Priority };
inline const char* word(CheckKind c) {
    switch (c) {
        case CheckKind::None:     return "";
        case CheckKind::Unique:   return "unique";
        case CheckKind::Unique0:  return "unique0";
        case CheckKind::Priority: return "priority";
    }
    SLANG_UNREACHABLE;
}

enum class EventKind : uint8_t { Sensitivity, Wait };
inline const char* word(EventKind k) {
    return k == EventKind::Sensitivity ? "sensitivity" : "wait";
}

/// None spells the NULL of a level-sensitive event written explicitly.
/// Named Edge because slang has an ast::EdgeKind these headers see
/// unqualified.
enum class Edge : uint8_t { None, Posedge, Negedge, Both };
inline const char* word(Edge e) {
    switch (e) {
        case Edge::None:    return "";
        case Edge::Posedge: return "posedge";
        case Edge::Negedge: return "negedge";
        case Edge::Both:    return "both";
    }
    SLANG_UNREACHABLE;
}

enum class StmtKind : uint8_t {
    Assignment, Assertion, Wait, Call, SystemTask, EventControl, Alias, Release,
    Trigger, Disable
};
inline const char* word(StmtKind k) {
    switch (k) {
        case StmtKind::Assignment:   return "assignment";
        case StmtKind::Assertion:    return "assertion";
        case StmtKind::Wait:         return "wait";
        case StmtKind::Call:         return "call";
        case StmtKind::SystemTask:   return "system_task";
        case StmtKind::EventControl: return "event_control";
        case StmtKind::Alias:        return "alias";
        case StmtKind::Release:      return "release";
        case StmtKind::Trigger:      return "trigger";
        case StmtKind::Disable:      return "disable";
    }
    SLANG_UNREACHABLE;
}

/// None spells the NULL of a statement that is not an assignment.
enum class AssignKind : uint8_t { None, Continuous, Blocking, Nonblocking };
inline const char* word(AssignKind k) {
    switch (k) {
        case AssignKind::None:        return "";
        case AssignKind::Continuous:  return "continuous";
        case AssignKind::Blocking:    return "blocking";
        case AssignKind::Nonblocking: return "nonblocking";
    }
    SLANG_UNREACHABLE;
}

/// module.def_kind. No Checker: nothing in the extractor can produce one.
enum class DefKind : uint8_t { Module, Interface, Program, Package };
inline const char* word(DefKind k) {
    switch (k) {
        case DefKind::Module:    return "module";
        case DefKind::Interface: return "interface";
        case DefKind::Program:   return "program";
        case DefKind::Package:   return "package";
    }
    SLANG_UNREACHABLE;
}

enum class NodeKind : uint8_t {
    Root, Instance, Generate, Primitive, Unresolved, Package
};
inline const char* word(NodeKind k) {
    switch (k) {
        case NodeKind::Root:       return "root";
        case NodeKind::Instance:   return "instance";
        case NodeKind::Generate:   return "generate";
        case NodeKind::Primitive:  return "primitive";
        case NodeKind::Unresolved: return "unresolved";
        case NodeKind::Package:    return "package";
    }
    SLANG_UNREACHABLE;
}

enum class ProcKind : uint8_t {
    Always, AlwaysFF, AlwaysComb, AlwaysLatch, Initial, Final
};
inline const char* word(ProcKind k) {
    switch (k) {
        case ProcKind::Always:      return "always";
        case ProcKind::AlwaysFF:    return "always_ff";
        case ProcKind::AlwaysComb:  return "always_comb";
        case ProcKind::AlwaysLatch: return "always_latch";
        case ProcKind::Initial:     return "initial";
        case ProcKind::Final:       return "final";
    }
    SLANG_UNREACHABLE;
}

enum class TermKind : uint8_t { Signal, Interface };
inline const char* word(TermKind k) {
    return k == TermKind::Signal ? "signal" : "interface";
}

/// None spells the NULL of an interface terminal or an unresolved
/// instance's terminal, which have no direction of their own.
enum class Direction : uint8_t { None, Input, Output, Inout, Ref };
inline const char* word(Direction d) {
    switch (d) {
        case Direction::None:   return "";
        case Direction::Input:  return "input";
        case Direction::Output: return "output";
        case Direction::Inout:  return "inout";
        case Direction::Ref:    return "ref";
    }
    SLANG_UNREACHABLE;
}

enum class PrimKind : uint8_t { Gate, Switch, Udp };
inline const char* word(PrimKind k) {
    switch (k) {
        case PrimKind::Gate:   return "gate";
        case PrimKind::Switch: return "switch";
        case PrimKind::Udp:    return "udp";
    }
    SLANG_UNREACHABLE;
}

enum class ConnKind : uint8_t {
    Signal, Constant, Unconnected, ExpressionOperand, Interface,
    ExternalReference
};
inline const char* word(ConnKind k) {
    switch (k) {
        case ConnKind::Signal:            return "signal";
        case ConnKind::Constant:          return "constant";
        case ConnKind::Unconnected:       return "unconnected";
        case ConnKind::ExpressionOperand: return "expression_operand";
        case ConnKind::Interface:         return "interface";
        case ConnKind::ExternalReference: return "external_reference";
    }
    SLANG_UNREACHABLE;
}

} // namespace designdb::detail
