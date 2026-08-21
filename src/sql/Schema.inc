// The schema is *instance-level*: rows hang off `inst`, the elaborated
// occurrence, not off a folded module variant. Thirty-two copies of one core
// are thirty-two sets of rows, and in exchange every net, terminal and
// statement is one row with one id -- "who drives bit 3 of THIS instance's
// q" is an indexed lookup, and a fan-in cone is a recursive CTE over
// `net_dep` rather than an application-side walk that re-folds the hierarchy
// at every boundary. The fold was v9's trade; this is the other side of it,
// taken deliberately (see doc/designdb-schema.md for the accounting).
//
// Naming rules, so a column can be guessed rather than looked up:
//
//   * Tables are singular_snake_case. `_id` appears exactly where a column
//     holds another table's primary key (`inst.module_id` -> module.id), and
//     nowhere else -- an attribute is never dressed as a key.
//   * A bit range is prefixed with the end it describes (`src_lo`,
//     `tgt_hi`, `term_exact`); a single-range table spells its own range
//     bare (`lo`/`hi`/`is_exact`). `map_exact` always describes the
//     correspondence BETWEEN two ends, never either end's own range.
//   * Kinds and directions are their words (`'input'`, `'constant'`,
//     `'always_ff'`), never integer codes. Three values never deserved the
//     decode table every consumer had to carry; CHECK constraints hold the
//     closed sets.
//   * `ordinal` is position in a declaration or extraction list; `sequence`
//     is execution order inside a procedure. Neither is an identity.
//
// Ranges keep the v7 encoding everywhere: LSB-relative offsets into the
// flattened object, NOT declared indices. `logic [15:8] off` has bit 15 at
// offset 7. NULL bits with exact=1 is the whole object; NULL bits with
// exact=0 is somewhere inside it, unknown where; a present range with
// exact=0 is an upper bound rather than the bits actually touched.
//
// The REFERENCES clauses document the joins; they are not enforced at runtime,
// since `PRAGMA foreign_keys` is left at its default of off. The verifier runs
// `foreign_key_check` on every export -- the ids are all issued by one process
// in one pass, and paying a lookup per inserted row to re-verify them is the
// wrong side of that trade.
R"SQL(
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);

CREATE TABLE src_file(
    id      INTEGER PRIMARY KEY,
    path    TEXT UNIQUE,
    digest  TEXT);

-- `path` is the spelling the rows carry -- as written in the filelist or on
-- the command line -- while `src_file.path` is absolute. The two genuinely
-- differ, so `src_file_id` joins them: without it a consumer checking
-- digests had to match spellings by basename, which breaks on the first
-- design with two files of one name.
CREATE TABLE file(
    id             INTEGER PRIMARY KEY,
    path           TEXT UNIQUE,
    src_file_id    INTEGER REFERENCES src_file(id));

-- Repeated type text, interned -- the one intern table v10 keeps. A packed
-- struct or enum prints as its entire member list, far larger than the row
-- referencing it, and the instance-level model repeats each declaration once
-- per occurrence. Names are not interned: they are short, and the join every
-- query paid the old `name` table was the storage tail wagging the schema.
CREATE TABLE data_type(id INTEGER PRIMARY KEY, text TEXT UNIQUE);

-- A source definition: what was written, not what it elaborated into. The
-- parameter values a body elaborated with are per-occurrence facts and live
-- on `inst.param_signature` -- v9 folded them into module identity, v10
-- puts identity where the source has it. (name, file_id, line) is the
-- definition's own identity; two libraries may define one name.
CREATE TABLE module(
    id              INTEGER PRIMARY KEY,
    name            TEXT NOT NULL,
    def_kind        TEXT NOT NULL
        /*!*/CHECK(def_kind IN ('module','interface','program','checker','package'))/*!*/,
    file_id         INTEGER REFERENCES file(id),
    line            INTEGER,
    col             INTEGER,
    UNIQUE(name, file_id, line));

-- The hierarchy tree, one node per level, one path segment per node --
-- resolving `a.b[0].c` is one indexed lookup per segment, which is why the
-- paths themselves are never stored. This is the supertype: a module
-- instance is a `tree_node` plus an `inst` row under the same id, a gate is
-- a `tree_node` plus a `primitive` row, a generate block is a bare
-- `tree_node`. node_kind says which, and the verifier holds the bijection.
--
--   root        the top instance; it has an `inst` row and no parent.
--   instance    a resolved module/interface/program instance -> `inst`.
--   generate    a generate block or one element of a generate array. A
--               naming level, not an object; nothing subtypes it.
--   primitive   a gate, switch or UDP -> `primitive`.
--   unresolved  an instance whose definition slang could not find -> `inst`
--               with module_id NULL. A trace really does stop here.
--   package     a package (or the compilation unit's `$unit` scope), a
--               pseudo-occurrence above the roots -> `inst` with
--               parent_inst_id NULL and a `module` of def_kind 'package'.
--               Its variables are `net` rows, so a `pkg::x` reference
--               resolves to a real object instead of leaving the model.
CREATE TABLE tree_node(
    id             INTEGER PRIMARY KEY,
    parent_node_id INTEGER REFERENCES tree_node(id),
    name           TEXT NOT NULL,
    node_kind      TEXT NOT NULL
        /*!*/CHECK(node_kind IN ('root','instance','generate','primitive','unresolved','package'))/*!*/,
    ordinal        INTEGER NOT NULL);

-- One module instance occurrence. `id` IS the tree_node id -- one id space
-- for the whole hierarchy, so `net.inst_id` and `tree_node.parent_node_id`
-- never disagree about what an instance is.
--
-- `parent_inst_id` is the nearest enclosing module instance, skipping
-- generate levels -- the ancestry `tree_node` already encodes, denormalised
-- one hop because every ownership check walks it. The verifier holds the two
-- encodings equal.
--
-- `param_signature` is the elaborated parameter values, normalised, in
-- declaration order, localparams included (it over-splits, never
-- under-splits). Instances of one module with one signature share a body and
-- carry identical rows -- the sharing v9 stored once, v10 stamps out.
CREATE TABLE inst(
    id                    INTEGER PRIMARY KEY REFERENCES tree_node(id),
    module_id             INTEGER REFERENCES module(id),
    parent_inst_id        INTEGER REFERENCES inst(id),
    param_signature       TEXT,
    unresolved_def        TEXT,
    file_id               INTEGER REFERENCES file(id),
    line                  INTEGER,
    col                   INTEGER);

-- param_signature made queryable: one row per elaborated parameter value,
-- declaration order, localparams and type parameters included -- the same
-- normalisation the signature is built from, and the verifier holds the
-- two representations equal. "Every instance with WIDTH=8" was a LIKE
-- over the signature (and matched XWIDTH=8); now it is an indexed seek.
CREATE TABLE inst_param(
    inst_id  INTEGER NOT NULL REFERENCES inst(id),
    ordinal  INTEGER NOT NULL,
    name     TEXT NOT NULL,
    value    TEXT NOT NULL,
    PRIMARY KEY(inst_id, ordinal));

-- One gate, switch or UDP instance. `id` IS the tree_node id. Not a module
-- instance (it has no body, no ports, no parameters) and not a statement
-- (nothing was assigned); its dataflow is `net_dep` rows with
-- prim_id set. Expression operators (`&`, `+`, `?:`) are not
-- primitives -- they stay inside their statement's own rows.
CREATE TABLE prim(
    id              INTEGER PRIMARY KEY REFERENCES tree_node(id),
    inst_id         INTEGER NOT NULL REFERENCES inst(id),
    prim_kind       TEXT NOT NULL /*!*/CHECK(prim_kind IN ('gate','switch','udp'))/*!*/,
    def_name        TEXT NOT NULL,
    file_id         INTEGER REFERENCES file(id),
    line            INTEGER,
    col             INTEGER);

-- One connectable object in one instance: a net or a variable -- anything
-- that can be driven, read or wired. Parameters, type parameters and
-- interface ports are NOT here: they are not connectable, and v9 mixing them
-- into `symbol` made every "signals of this scope" query filter them out.
--
-- Subroutine formals and locals ARE here (name is the scope-relative dotted
-- path, `bump.v`): a call binds actuals to formals and `net_dep` ends are
-- ids, so a formal without a row would be a dependency with a dangling end.
-- Their scope_node_id is the nearest tree_node, since a subroutine is not a
-- hierarchy level.
--
-- decl_kind is the net type's own word (`wire`, `wand`, `trireg`,
-- ...) or `variable`. is_implicit=1 marks a net slang created for an
-- undeclared identifier under the active `default_nettype`; its
-- decl_kind is that nettype, and its location is the first use.
CREATE TABLE net(
    id               INTEGER PRIMARY KEY,
    inst_id          INTEGER NOT NULL REFERENCES inst(id),
    scope_node_id    INTEGER NOT NULL REFERENCES tree_node(id),
    name             TEXT NOT NULL,
    decl_kind        TEXT NOT NULL,
    data_type_id     INTEGER REFERENCES data_type(id),
    width            INTEGER,
    is_implicit      INTEGER NOT NULL CHECK(is_implicit IN (0,1)),
    file_id          INTEGER REFERENCES file(id),
    line             INTEGER,
    col              INTEGER);

-- One terminal: a port on an instance's boundary. The root instance's
-- terminals are the design's top-level ports; a child's are the pins its
-- parent connects. One noun for both, because they are the same object seen
-- from the two sides v9 conflated in one `port` row.
--
-- direction is NULL for an interface terminal (it has none) and for a
-- terminal of an unresolved instance (nobody knows). is_const marks
-- `const ref`; NULL where constness does not apply. An unresolved instance
-- still gets terminal rows -- one per connection its parent wrote, named as
-- the connection names them -- so `net_conn` has something to bind and
-- "connected to a black box" stays distinct from "connected to nothing".
CREATE TABLE term(
    id            INTEGER PRIMARY KEY,
    inst_id       INTEGER NOT NULL REFERENCES inst(id),
    name          TEXT NOT NULL,
    term_kind     TEXT NOT NULL /*!*/CHECK(term_kind IN ('signal','interface'))/*!*/,
    direction     TEXT /*!*/CHECK(direction IN ('input','output','inout','ref'))/*!*/,
    data_type_id  INTEGER REFERENCES data_type(id),
    width         INTEGER,
    ordinal       INTEGER NOT NULL,
    is_const      INTEGER CHECK(is_const IN (0,1)),
    modport       TEXT,
    file_id       INTEGER REFERENCES file(id),
    line          INTEGER,
    col           INTEGER);

-- The INSIDE of a terminal: which nets of its own instance it stands for,
-- one row per segment. An ANSI port is one whole-to-whole row; a non-ANSI
-- `.p({hi, lo})` formal is one row per element, each with its window of the
-- terminal (`term_lo`/`term_hi`) and of the net. Both nets belong to the
-- terminal's own instance -- the outside is `net_conn`'s business, and
-- keeping the two relations apart is what v9's one-table version kept
-- getting wrong.
CREATE TABLE term_map(
    term_id       INTEGER NOT NULL REFERENCES term(id),
    ordinal       INTEGER NOT NULL,
    inner_net_id  INTEGER NOT NULL REFERENCES net(id),
    term_lo       INTEGER,
    term_hi       INTEGER,
    term_exact    INTEGER NOT NULL CHECK(term_exact IN (0,1)),
    inner_lo      INTEGER,
    inner_hi      INTEGER,
    inner_exact   INTEGER NOT NULL CHECK(inner_exact IN (0,1)),
    map_exact     INTEGER NOT NULL CHECK(map_exact IN (0,1)),
    PRIMARY KEY(term_id, ordinal));

-- The OUTSIDE of a terminal: what the parent connected to it, one row per
-- atomic segment -- each concatenation element and each replication copy is
-- its own row with its own window of the formal, so `.q({2{r}})` is two
-- rows whose windows tile q. conn_kind decides which columns apply:
--
--   signal              outer_net_id names a net of the PARENT instance.
--   constant            a tie-off; no net, but the window is kept so the
--                       formal's bits tile rather than leaving a gap that
--                       reads as an exporter bug.
--   unconnected         nobody connected it. Recorded, not omitted: absence
--                       would also mean "the exporter did not get this far".
--   expression_operand  the actual is an expression; this row is one net it
--                       reads. `.en(state == RUN)` samples state but does
--                       not alias it to en -- map_exact is 0 by
--                       construction.
--   interface           outer_intf_inst_id names the bound interface
--                       instance. No dataflow arc pretends to cross here.
--   external_reference  tied to something with no name in the parent
--                       (`.p(u.g[7:4])`); outer_hier_ref_id says what, and
--                       the arc crosses once that reference resolves.
--
-- Range discipline: columns describing an end that does not exist are NULL
-- -- a constant has no net end, an interface binding has no bit domain --
-- so a tie-off never reads as "the whole of nothing, exactly".
CREATE TABLE net_conn(
    id                 INTEGER PRIMARY KEY,
    outer_net_id       INTEGER REFERENCES net(id),
    term_id            INTEGER NOT NULL REFERENCES term(id),
    ordinal            INTEGER NOT NULL,
    conn_kind          TEXT NOT NULL
        /*!*/CHECK(conn_kind IN ('signal','constant','unconnected',
                                  'expression_operand','interface','external_reference'))/*!*/,
    outer_lo           INTEGER,
    outer_hi           INTEGER,
    outer_exact        INTEGER CHECK(outer_exact IN (0,1)),
    term_lo            INTEGER,
    term_hi            INTEGER,
    term_exact         INTEGER CHECK(term_exact IN (0,1)),
    map_exact          INTEGER CHECK(map_exact IN (0,1)),
    outer_intf_inst_id INTEGER REFERENCES inst(id),
    outer_hier_ref_id  INTEGER REFERENCES hier_ref(id),
    file_id            INTEGER REFERENCES file(id),
    line               INTEGER,
    col                INTEGER,
    UNIQUE(term_id, ordinal));

-- One procedure: an always/initial/final block, or a task/function body.
-- v9 named these with a bare per-module integer that was not a key in any
-- table; this is the object that integer was gesturing at.
CREATE TABLE proc(
    id             INTEGER PRIMARY KEY,
    inst_id        INTEGER NOT NULL REFERENCES inst(id),
    scope_node_id  INTEGER NOT NULL REFERENCES tree_node(id),
    name           TEXT,
    proc_kind      TEXT NOT NULL
        /*!*/CHECK(proc_kind IN ('always','always_ff','always_comb','always_latch',
                                 'initial','final','task','function'))/*!*/,
    ordinal        INTEGER NOT NULL,
    file_id        INTEGER REFERENCES file(id),
    line           INTEGER,
    col            INTEGER);

-- One subroutine-body expansion. A task or function body is walked once per
-- call site (its statements carry that call's gating and delay), and this is
-- that site's identity, so a consumer can partition a fan-in cone by it and
-- never mix one call's gating with another's argument -- the combination no
-- single call makes, which a shared formal net otherwise admits. `stmt` and
-- `net_dep` rows a body walk produced name their site in `call_site_id`;
-- `parent_call_site_id` chains nested calls into a call string.
--
-- caller_stmt_id is the statement making the call (NULL for a call in a
-- control expression, which this schema attaches to no statement). depth is
-- the call-stack depth, 1 at the outermost call. Additive: a reader that
-- does not know call sites ignores the column and the table.
CREATE TABLE call_site(
    id                   INTEGER PRIMARY KEY,
    inst_id              INTEGER NOT NULL REFERENCES inst(id),
    caller_stmt_id       INTEGER REFERENCES stmt(id),
    parent_call_site_id  INTEGER REFERENCES call_site(id),
    subroutine_name      TEXT NOT NULL,
    depth                INTEGER NOT NULL);

-- One statement, or one statement-level construct. The statement is the
-- object; its targets, operands and other reads are child rows -- v9's
-- `assignment` was one row per TARGET, so `{a,b} = {x,y}` was two rows
-- claiming one statement and a statement writing only outward was no row at
-- all. Here it is one row regardless.
--
-- sequence is execution order within the procedure, NULL exactly when
-- proc_id is NULL (a continuous assign has no execution order).
-- assign_kind is NULL for non-assignments. delay is the delay control's
-- normalised source text (`#3`, `#(rise, fall)`), not a number this tool
-- pretended to evaluate; NULL when there is none.
--
-- What is deliberately NOT here, unchanged from v9: no branch-condition
-- text, no clocked flag, no source text.
--
-- `force` records as a blocking assignment whose construct says 'force'
-- (a procedural `assign` says 'proc_assign') -- the dataflow is a
-- blocking assignment's, and the word is what lets a debugger find the
-- hijack. `release`/`deassign` record as stmt_kind='release' with the
-- lvalue as a target row and deliberately NO dependency: nothing is
-- driven, and the row answers "where does the force end".
CREATE TABLE stmt(
    id                    INTEGER PRIMARY KEY,
    inst_id               INTEGER NOT NULL REFERENCES inst(id),
    scope_node_id         INTEGER NOT NULL REFERENCES tree_node(id),
    proc_id               INTEGER REFERENCES proc(id),
    ordinal               INTEGER NOT NULL,
    sequence              INTEGER,
    stmt_kind             TEXT NOT NULL
        /*!*/CHECK(stmt_kind IN ('assignment','assertion','wait','call',
                                 'system_task','event_control','alias',
                                 'release'))/*!*/,
    construct             TEXT,
    assign_kind           TEXT
        /*!*/CHECK(assign_kind IN ('continuous','blocking','nonblocking'))/*!*/,
    delay                 TEXT,
    dropped_operand_count INTEGER NOT NULL,
    call_site_id          INTEGER REFERENCES call_site(id),
    file_id               INTEGER REFERENCES file(id),
    line                  INTEGER,
    col                   INTEGER);

-- One target reference of an assignment (LHS), in written order. A target
-- whose net lies outside the instance is not here -- it is a `hier_ref`
-- with access='write' on the same stmt_id.
CREATE TABLE stmt_target(
    id       INTEGER PRIMARY KEY,
    stmt_id  INTEGER NOT NULL REFERENCES stmt(id),
    ordinal  INTEGER NOT NULL,
    net_id   INTEGER NOT NULL REFERENCES net(id),
    lo       INTEGER,
    hi       INTEGER,
    is_exact INTEGER NOT NULL CHECK(is_exact IN (0,1)),
    UNIQUE(stmt_id, ordinal));

-- One operand reference of an assignment's RHS. Operands belong to the
-- STATEMENT, not to a target: which operand feeds which target is
-- `net_dep`'s answer, and pairing them here is exactly the cross product
-- v7 removed. A read occurring twice is two rows.
CREATE TABLE assign_operand(
    id       INTEGER PRIMARY KEY,
    stmt_id  INTEGER NOT NULL REFERENCES stmt(id),
    ordinal  INTEGER NOT NULL,
    net_id   INTEGER NOT NULL REFERENCES net(id),
    lo       INTEGER,
    hi       INTEGER,
    is_exact INTEGER NOT NULL CHECK(is_exact IN (0,1)),
    UNIQUE(stmt_id, ordinal));

-- One statement read that is not an assignment operand, classified by role:
--
--   control        a branch condition the statement's targets sit under
--   assertion      read by an assert/assume/cover/expect
--   wait           a wait statement's condition
--   event          a sensitivity/event expression that is not a plain net
--                  (those are proc_event rows -- one read, one table, never
--                  both, or v_load counts it twice)
--   call_argument  an actual at a call site
--   system_task    read by $display and friends
CREATE TABLE expr_ref(
    id       INTEGER PRIMARY KEY,
    stmt_id  INTEGER NOT NULL REFERENCES stmt(id),
    ordinal  INTEGER NOT NULL,
    net_id   INTEGER NOT NULL REFERENCES net(id),
    role     TEXT NOT NULL
        /*!*/CHECK(role IN ('control','assertion','wait','event',
                       'call_argument','system_task'))/*!*/,
    lo       INTEGER,
    hi       INTEGER,
    is_exact INTEGER NOT NULL CHECK(is_exact IN (0,1)),
    UNIQUE(stmt_id, ordinal));

-- One edge event a procedure triggers on or waits on. All of them, because
-- an event list has no order and recording "the first" records how the
-- author arranged the list. Which is the clock is deliberately not decided
-- here, exactly as in v9.
--
-- event_kind='sensitivity' rows belong to the procedure header (stmt_id
-- NULL); event_kind='wait' rows are statement-level controls reached during
-- execution (stmt_id set). net_id is NULL when the event expression is not
-- a plain net -- the reads of such an expression are expr_ref rows with
-- role='event', never both.
CREATE TABLE proc_event(
    id           INTEGER PRIMARY KEY,
    proc_id      INTEGER NOT NULL REFERENCES proc(id),
    stmt_id      INTEGER REFERENCES stmt(id),
    net_id       INTEGER REFERENCES net(id),
    event_kind   TEXT NOT NULL /*!*/CHECK(event_kind IN ('sensitivity','wait'))/*!*/,
    edge_kind    TEXT /*!*/CHECK(edge_kind IN ('posedge','negedge','both'))/*!*/,
    file_id      INTEGER REFERENCES file(id),
    line         INTEGER,
    col          INTEGER);

-- One net-to-net dependency occurrence -- the adjacency list v_driver and
-- v_load index, and the provenance record v9's deduplicated `edge` erased.
-- NOT deduplicated: the same source reaching the same target from two
-- statements is two rows, each naming its statement. `{a,b} = {x,y}` is
-- x->a and y->b, each naming its operand and target rows -- never the
-- four-way cross product.
--
-- dep_kind, and what must be set for each (verifier-enforced):
--
--   data       an assignment moves it: stmt_id, and per end either the
--              local reference (assign_operand_id / stmt_target_id) or
--              the hierarchical one (src_hier_ref_id / tgt_hier_ref_id).
--              src_net_id NULL with no source reference of either kind is
--              a constant driver (`q <= 8'h0`) -- the row still names the
--              statement, which is what a driver query reports; every
--              src_* column is NULL with it. src_net_id NULL WITH
--              src_hier_ref_id is an external driver: the reference did
--              not resolve to a net row, the source range survives, and
--              v_driver reports 'external'.
--   control    it reaches the target through a branch condition: stmt_id,
--              the condition's expr_ref_id (role='control') or
--              src_hier_ref_id, and the target's reference as above.
--              map_exact is 0 -- a condition gates, it does not map.
--   primitive  a gate/switch/UDP couples them: prim_id, per LRM
--              (input, output) pairing.
--   procedure  a call binds them: actual to formal by argument direction,
--              and formal back to a written actual. stmt_id is the calling
--              statement (NULL for a call in a control expression); the
--              actual's read is expr_ref_id (role='call_argument') or
--              src_hier_ref_id.
--
-- A dependency whose end lies in ANOTHER instance -- `assign q = u.x;`,
-- a write through an interface port -- carries the resolved net id like
-- any other end, and names the hier_ref row it resolved through instead of
-- an operand/target row. The pairing is made where the statement was
-- walked, one row per (source element, target element) that share bits --
-- never by joining hier_ref to operands on stmt_id afterwards, which would
-- resurrect the cross product v7 removed. An unresolved TARGET reference
-- produces no dependency: tgt_net_id is NOT NULL, and a guessed written
-- object would be a wrong fact. An unresolved SOURCE reference keeps its
-- row -- src_net_id NULL, src_hier_ref_id set -- because dropping it made
-- "driven through a name this export cannot resolve" indistinguishable
-- from "undriven", which is a wrong fact of the quieter kind.
--
-- src_net_id/tgt_net_id repeat what the referenced rows already
-- know. Deliberate, verified redundancy: this table is the driver/load
-- index, and the verifier holds the copies equal.
CREATE TABLE net_dep(
    id                 INTEGER PRIMARY KEY,
    src_net_id         INTEGER REFERENCES net(id),
    tgt_net_id         INTEGER NOT NULL REFERENCES net(id),
    stmt_id            INTEGER REFERENCES stmt(id),
    assign_operand_id  INTEGER REFERENCES assign_operand(id),
    stmt_target_id   INTEGER REFERENCES stmt_target(id),
    expr_ref_id        INTEGER REFERENCES expr_ref(id),
    prim_id            INTEGER REFERENCES prim(id),
    src_hier_ref_id    INTEGER REFERENCES hier_ref(id),
    tgt_hier_ref_id    INTEGER REFERENCES hier_ref(id),
    dep_kind           TEXT NOT NULL
        /*!*/CHECK(dep_kind IN ('data','control','primitive','procedure','alias'))/*!*/,
    src_lo             INTEGER,
    src_hi             INTEGER,
    src_exact          INTEGER CHECK(src_exact IN (0,1)),
    tgt_lo             INTEGER,
    tgt_hi             INTEGER,
    tgt_exact          INTEGER NOT NULL CHECK(tgt_exact IN (0,1)),
    map_exact          INTEGER CHECK(map_exact IN (0,1)),
    call_site_id       INTEGER REFERENCES call_site(id));

-- One reference that leaves the instance: an XMR, an interface member, a
-- package item. The path is stored as written (normalised), AND -- new in
-- v10, because an occurrence knows its place in the hierarchy where a
-- folded row could not -- resolved to the target instance and net when
-- slang could resolve it and the target is in this export. NULL
-- resolved_* is "not resolved here", never a fabricated object.
--
-- access: read | write | connect (a port connection tied outward; its
-- net_conn row points back here).
CREATE TABLE hier_ref(
    id               INTEGER PRIMARY KEY,
    inst_id          INTEGER NOT NULL REFERENCES inst(id),
    stmt_id          INTEGER REFERENCES stmt(id),
    path             TEXT NOT NULL,
    access           TEXT NOT NULL /*!*/CHECK(access IN ('read','write','connect'))/*!*/,
    resolved_inst_id INTEGER REFERENCES inst(id),
    resolved_net_id  INTEGER REFERENCES net(id),
    lo               INTEGER,
    hi               INTEGER,
    is_exact         INTEGER NOT NULL CHECK(is_exact IN (0,1)),
    file_id          INTEGER REFERENCES file(id),
    line             INTEGER,
    col              INTEGER);
)SQL"
