// Copyright (c) 2026 neveltyc
// released under the BSD 3-Clause License (see LICENSE)
//
// The closed vocabularies, as enums, with their published spellings in one
// place. Every value here is interface: it appears in a schema CHECK clause,
// in doc/designdb-schema.md, and in verify-designdb.py's DOMAINS table --
// which is the authoritative statement of the contract and which
// --domain-coverage holds the fixture corpus to. Before this header the
// spellings were string literals scattered across four files, and the only
// thing catching a typo was the CHECK clause at insert time (when enabled);
// a value the extractor could never produce, like the v16 'checker', was
// invisible until the corpus-coverage gate existed.
//
// Template rows store the enum; the word appears exactly once, where the
// stamper turns a row into SQL. Domains the schema publishes as OPEN --
// stmt.construct (a system task's own name), net.decl_kind (a user-defined
// nettype's own name) -- stay strings on purpose.

#pragma once

#include <cstdint>

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
    return "";
}

enum class Access : uint8_t { Read, Write, Connect };
inline const char* word(Access a) {
    switch (a) {
        case Access::Read:    return "read";
        case Access::Write:   return "write";
        case Access::Connect: return "connect";
    }
    return "";
}

enum class RefRole : uint8_t {
    Control, Assertion, Wait, Event, CallArgument, SystemTask
};
inline const char* word(RefRole r) {
    switch (r) {
        case RefRole::Control:      return "control";
        case RefRole::Assertion:    return "assertion";
        case RefRole::Wait:         return "wait";
        case RefRole::Event:        return "event";
        case RefRole::CallArgument: return "call_argument";
        case RefRole::SystemTask:   return "system_task";
    }
    return "";
}

enum class EventKind : uint8_t { Sensitivity, Wait };
inline const char* word(EventKind k) {
    return k == EventKind::Sensitivity ? "sensitivity" : "wait";
}

/// None spells the NULL of a level-sensitive event written explicitly.
/// Named Edge, not EdgeKind: slang has an ast::EdgeKind of its own and
/// these headers use its namespace unqualified.
enum class Edge : uint8_t { None, Posedge, Negedge, Both };
inline const char* word(Edge e) {
    switch (e) {
        case Edge::None:    return "";
        case Edge::Posedge: return "posedge";
        case Edge::Negedge: return "negedge";
        case Edge::Both:    return "both";
    }
    return "";
}

enum class StmtKind : uint8_t {
    Assignment, Assertion, Wait, Call, SystemTask, EventControl, Alias, Release
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
    }
    return "";
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
    return "";
}

/// module.def_kind. There is no Checker: the schema's CHECK clause listed
/// one at v16 and nothing could ever write it -- which is the kind of dead
/// interface value a closed enum makes visible.
enum class DefKind : uint8_t { Module, Interface, Program, Package };
inline const char* word(DefKind k) {
    switch (k) {
        case DefKind::Module:    return "module";
        case DefKind::Interface: return "interface";
        case DefKind::Program:   return "program";
        case DefKind::Package:   return "package";
    }
    return "";
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
    return "";
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
    return "";
}

enum class PrimKind : uint8_t { Gate, Switch, Udp };
inline const char* word(PrimKind k) {
    switch (k) {
        case PrimKind::Gate:   return "gate";
        case PrimKind::Switch: return "switch";
        case PrimKind::Udp:    return "udp";
    }
    return "";
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
    return "";
}

} // namespace designdb::detail
