// The stable query interface: fifteen views, the consumption contract.
// Their existence, column sets and order, column semantics, NULL rules and
// row granularity are what a consumer may rely on; changing any of those --
// adding a column included, since the version integer is the only capability
// signal a consumer has -- bumps the schema version, changing only how one is
// computed does not.
// verify-designdb.py asserts all of it on every export.
//
// Ground rules, revised from v8 for the instance-level model:
//
//   * A FACT view's row is one base-table row -- v_tree_node, v_net,
//     v_term, v_term_map, v_net_conn, v_net_dep,
//     v_stmt, v_stmt_target, v_stmt_operand, v_call_site, v_hier_ref -- and
//     the verifier checks count(view) == count(base). Every internal join is
//     against a primary key, so nothing fans out.
//   * v_driver and v_load are COMPOSITE: UNION ALL branches discriminated by
//     driver_kind/load_kind, each branch's row count reconcilable against
//     its base tables by a formula the verifier evaluates. They are the one
//     place the hierarchy crossing is composed (net_conn against term_map);
//     v9 left that composition to the consumer, and every consumer wrote it
//     differently or wrongly.
//   * v_conn_arc is scaffolding for that composition, NOT part of the
//     contract: consumers must not query it, and it may change or vanish
//     without a version bump. It exists because the same composition feeds
//     four branches and inlining it four times would guarantee drift.
//   * Explicit column lists, never SELECT *. No transitive closure -- a
//     fan-in cone is the consumer's recursive query over net_dep, one step
//     per row here.
//   * Plain CREATE VIEW, not IF NOT EXISTS: the writer only ever creates a
//     fresh database, so a name collision is a bug to fail on.
//
// SQLite resolves a view's column references when the view is *queried*, not
// when it is created -- the verifier's row-count checks query every view,
// which is what makes a stale view a caught error rather than a consumer's
// surprise.
R"SQL(
-- The meta table pivoted to one fixed row, so "which schema, which status" is
-- one SELECT with no key-value handling. Counts are CAST so a consumer gets
-- integers, not the TEXT the key-value table stores. The view only reshapes;
-- required-key enforcement stays in the verifier.
CREATE VIEW v_db_info AS
SELECT
    CAST(MAX(CASE WHEN key = 'schema_version'
                  THEN value END) AS INTEGER) AS schema_version,
    MAX(CASE WHEN key = 'tool_version'          THEN value END) AS tool_version,
    MAX(CASE WHEN key = 'slang_version'         THEN value END) AS slang_version,
    MAX(CASE WHEN key = 'producer_revision'     THEN value END) AS producer_revision,
    MAX(CASE WHEN key = 'top'                   THEN value END) AS top,
    MAX(CASE WHEN key = 'analysis_status'       THEN value END) AS analysis_status,
    CAST(MAX(CASE WHEN key = 'error_count'
                  THEN value END) AS INTEGER) AS error_count,
    CAST(MAX(CASE WHEN key = 'unresolved_count'
                  THEN value END) AS INTEGER) AS unresolved_count,
    CAST(MAX(CASE WHEN key = 'empty_procedure_count'
                  THEN value END) AS INTEGER) AS empty_procedure_count,
    CAST(MAX(CASE WHEN key = 'duplicate_path_count'
                  THEN value END) AS INTEGER) AS duplicate_path_count,
    MAX(CASE WHEN key = 'config_digest'         THEN value END) AS config_digest
FROM meta;

-- One row per tree_node. The subtype columns are NULL by node_kind, and that
-- is the contract: root/instance/unresolved have inst_id (and module_id
-- unless unresolved); primitive and generate have inst_id NULL. For a
-- primitive, parent_inst_id is the instance whose body wrote it and
-- def_name is the gate/UDP name; for an unresolved node,
-- def_name is the unresolvable spelling. Location is the
-- instantiation site; the root and generate levels have none.
CREATE VIEW v_tree_node AS
SELECT
    n.id                                   AS node_id,
    n.parent_node_id                       AS parent_node_id,
    n.name                                 AS node_name,
    n.node_kind                            AS node_kind,
    n.ordinal                              AS ordinal,
    i.id                                   AS inst_id,
    COALESCE(i.parent_inst_id, p.inst_id)  AS parent_inst_id,
    i.module_id                            AS module_id,
    m.name                                 AS module_name,
    i.param_signature                      AS param_signature,
    COALESCE(p.def_name, i.unresolved_def) AS def_name,
    f.path                                 AS file_path,
    sf.path                                AS src_path,
    COALESCE(i.line, p.line)               AS src_line,
    COALESCE(i.col, p.col)                 AS src_col
FROM tree_node n
LEFT JOIN inst i         ON i.id = n.id
LEFT JOIN prim p    ON p.id = n.id
LEFT JOIN module m       ON m.id = i.module_id
LEFT JOIN file f         ON f.id = COALESCE(i.file_id, p.file_id)
LEFT JOIN src_file sf ON sf.id = f.src_file_id;

-- One row per net: a connectable object of one concrete instance. No
-- direction column -- direction belongs to terminals, and a net's port-ness
-- is one v_term_map join away. file_path is the spelling as written in
-- the filelist, src_path the absolute path it resolved to; they answer
-- different questions and neither substitutes for the other.
CREATE VIEW v_net AS
SELECT
    nt.id             AS net_id,
    nt.inst_id        AS inst_id,
    i.module_id       AS module_id,
    m.name            AS module_name,
    i.param_signature AS param_signature,
    nt.scope_node_id  AS scope_node_id,
    nt.name           AS net_name,
    nt.decl_kind      AS decl_kind,
    dt.text           AS data_type,
    nt.width          AS width,
    nt.is_implicit    AS is_implicit,
    f.path            AS file_path,
    sf.path           AS src_path,
    nt.line           AS src_line,
    nt.col            AS src_col
FROM net nt
JOIN inst i              ON i.id = nt.inst_id
LEFT JOIN module m       ON m.id = i.module_id
LEFT JOIN data_type dt   ON dt.id = nt.data_type_id
LEFT JOIN file f         ON f.id = nt.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id;

-- One row per terminal. direction NULL is an interface terminal or a
-- terminal of an unresolved instance -- term_kind and the owning
-- instance's node_kind say which.
CREATE VIEW v_term AS
SELECT
    t.id        AS term_id,
    t.inst_id   AS inst_id,
    i.module_id AS module_id,
    m.name      AS module_name,
    t.name      AS term_name,
    t.term_kind AS term_kind,
    t.direction AS direction,
    dt.text     AS data_type,
    t.width     AS width,
    t.ordinal   AS ordinal,
    t.is_const  AS is_const,
    t.modport   AS modport,
    f.path      AS file_path,
    sf.path     AS src_path,
    t.line      AS src_line,
    t.col       AS src_col
FROM term t
JOIN inst i              ON i.id = t.inst_id
LEFT JOIN module m       ON m.id = i.module_id
LEFT JOIN data_type dt   ON dt.id = t.data_type_id
LEFT JOIN file f         ON f.id = t.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id;

-- One row per term_map segment: the inside of a terminal.
CREATE VIEW v_term_map AS
SELECT
    mp.term_id      AS term_id,
    t.inst_id       AS term_inst_id,
    t.name          AS term_name,
    mp.ordinal      AS map_ordinal,
    mp.inner_net_id AS inner_net_id,
    n.name          AS inner_net_name,
    mp.term_lo      AS term_lo,
    mp.term_hi      AS term_hi,
    mp.term_exact   AS term_exact,
    mp.inner_lo     AS inner_lo,
    mp.inner_hi     AS inner_hi,
    mp.inner_exact  AS inner_exact,
    mp.map_exact    AS map_exact
FROM term_map mp
JOIN term t ON t.id = mp.term_id
JOIN net n  ON n.id = mp.inner_net_id;

-- One row per net_conn segment: the outside of a terminal, exactly as
-- written in the parent. Deliberately NOT composed with term_map -- this
-- view is the fact, v_driver/v_load are the composition.
CREATE VIEW v_net_conn AS
SELECT
    c.id                 AS conn_id,
    c.outer_net_id       AS outer_net_id,
    pn.inst_id           AS outer_inst_id,
    pn.name              AS outer_net_name,
    c.term_id            AS term_id,
    t.inst_id            AS term_inst_id,
    t.name               AS term_name,
    t.direction          AS direction,
    c.conn_kind          AS conn_kind,
    c.ordinal            AS ordinal,
    c.outer_lo           AS outer_lo,
    c.outer_hi           AS outer_hi,
    c.outer_exact        AS outer_exact,
    c.term_lo            AS term_lo,
    c.term_hi            AS term_hi,
    c.term_exact         AS term_exact,
    c.map_exact          AS map_exact,
    c.outer_intf_inst_id AS outer_intf_inst_id,
    c.outer_hier_ref_id  AS outer_hier_ref_id,
    f.path               AS file_path,
    sf.path              AS src_path,
    c.line               AS src_line,
    c.col                AS src_col
FROM net_conn c
JOIN term t              ON t.id = c.term_id
LEFT JOIN net pn         ON pn.id = c.outer_net_id
LEFT JOIN file f         ON f.id = c.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id;

-- One row per net_dep: a statement- or primitive-level dependency
-- occurrence, not deduplicated across statements. Location is the
-- statement's, or the primitive's for a primitive arc.
CREATE VIEW v_net_dep AS
SELECT
    d.id                     AS dep_id,
    d.src_net_id             AS src_net_id,
    sn.inst_id               AS src_inst_id,
    sn.name                  AS src_name,
    d.src_lo                 AS src_lo,
    d.src_hi                 AS src_hi,
    d.src_exact              AS src_exact,
    d.tgt_net_id             AS tgt_net_id,
    tn.inst_id               AS tgt_inst_id,
    tn.name                  AS tgt_name,
    d.tgt_lo                 AS tgt_lo,
    d.tgt_hi                 AS tgt_hi,
    d.tgt_exact              AS tgt_exact,
    d.stmt_id                AS stmt_id,
    d.assign_operand_id      AS assign_operand_id,
    d.stmt_target_id       AS stmt_target_id,
    d.expr_ref_id            AS expr_ref_id,
    d.prim_id                AS prim_id,
    d.src_hier_ref_id        AS src_hier_ref_id,
    d.tgt_hier_ref_id        AS tgt_hier_ref_id,
    d.dep_kind               AS dep_kind,
    d.map_exact              AS map_exact,
    d.call_site_id           AS call_site_id,
    f.path                   AS file_path,
    sf.path                  AS src_path,
    COALESCE(s.line, p.line) AS src_line,
    COALESCE(s.col, p.col)   AS src_col
FROM net_dep d
JOIN net tn              ON tn.id = d.tgt_net_id
LEFT JOIN net sn         ON sn.id = d.src_net_id
LEFT JOIN stmt s         ON s.id = d.stmt_id
LEFT JOIN prim p    ON p.id = d.prim_id
LEFT JOIN file f         ON f.id = COALESCE(s.file_id, p.file_id)
LEFT JOIN src_file sf ON sf.id = f.src_file_id;

-- NOT part of the stable contract. The composition v_driver and v_load
-- project: one row per overlapping (net_conn segment, term_map segment)
-- pair -- a terminal's outside met with its inside. Windows are intersected
-- on the terminal; a side's net range is narrowed through the overlap when
-- every link of that side's chain is exact, passed through untouched when
-- the other side does not narrow it, and degraded to exact=0 otherwise --
-- the range is then an upper bound, which is what exact=0 means everywhere.
-- outer_ref rides along for the outer end that was named hierarchically
-- (`.p(u.g[7:4])`): the join that resolves such a tie to its net is already
-- here, and both composite views want the spelling beside the net.
CREATE VIEW v_conn_arc AS
-- The outer end is either the connection's own net or, for a tie whose
-- reference resolved, the net that reference landed on. Deriving it with
-- COALESCE across the two tables read well and cost every consumer dearly:
-- a value that may come from either table can be pushed down to neither, so
-- `WHERE signal_net_id = ?` scanned all of net_conn instead of seeking
-- net_conn_by_net. On a design whose clock reaches a few thousand nets, a
-- trace walks one such query per hop.
--
-- The two cases are disjoint on `c.outer_net_id IS NULL` -- which is exactly what
-- COALESCE tests -- so splitting there leaves outer_net_id a plain column
-- reference in each branch and the indexes usable again. The second branch
-- keeps the LEFT JOIN: a constant tie-off has neither a net nor a
-- reference, and must still appear with a NULL outer end, since it drives
-- the inner net.
WITH seg AS (
    SELECT
        c.id                                   AS conn_id,
        c.conn_kind                            AS conn_kind,
        c.outer_net_id                         AS outer_net_id,
        -- Always NULL in practice: this branch is the connection's OWN net,
        -- and the exporter writes outer_net_id and outer_hier_ref_id
        -- exclusively. Spelled as the column rather than a literal NULL on
        -- purpose -- a constant here stops SQLite flattening the compound
        -- subquery, and `WHERE signal_net_id = ?` on v_driver goes from a
        -- seek to a scan of seg. The verifier's plan check catches it, but
        -- the reason belongs where the temptation is.
        hr.path                                AS outer_ref,
        c.outer_lo                             AS c_net_lo,
        c.outer_hi                             AS c_net_hi,
        c.outer_exact                          AS c_net_exact,
        c.term_lo                              AS c_lo,
        c.term_hi                              AS c_hi,
        c.term_exact                           AS c_term_exact,
        c.map_exact                            AS c_map,
        c.file_id                              AS file_id,
        c.line                                 AS line,
        c.col                                  AS col,
        t.id                                   AS term_id,
        t.inst_id                              AS term_inst_id,
        t.direction                            AS direction,
        mp.inner_net_id                        AS inner_net_id,
        mp.term_lo                             AS m_lo,
        mp.term_hi                             AS m_hi,
        mp.term_exact                          AS m_term_exact,
        mp.inner_lo                            AS m_net_lo,
        mp.inner_hi                            AS m_net_hi,
        mp.inner_exact                         AS m_net_exact,
        mp.map_exact                           AS m_map
    FROM net_conn c
    JOIN term t      ON t.id = c.term_id
    JOIN term_map mp ON mp.term_id = c.term_id
    LEFT JOIN hier_ref hr ON hr.id = c.outer_hier_ref_id
    WHERE c.conn_kind IN ('signal', 'expression_operand', 'constant',
                                'external_reference')
      AND (c.conn_kind != 'external_reference'
           OR hr.resolved_net_id IS NOT NULL)
      AND (c.conn_kind != 'expression_operand'
           OR c.outer_net_id IS NOT NULL OR hr.resolved_net_id IS NOT NULL)
      AND (c.term_lo IS NULL OR mp.term_hi IS NULL OR c.term_lo <= mp.term_hi)
      AND (mp.term_lo IS NULL OR c.term_hi IS NULL OR mp.term_lo <= c.term_hi)
      AND c.outer_net_id IS NOT NULL
    UNION ALL
    SELECT
        c.id                                   AS conn_id,
        c.conn_kind                            AS conn_kind,
        hr.resolved_net_id                     AS outer_net_id,
        hr.path                                AS outer_ref,
        hr.lo                                  AS c_net_lo,
        hr.hi                                  AS c_net_hi,
        hr.is_exact                            AS c_net_exact,
        c.term_lo                              AS c_lo,
        c.term_hi                              AS c_hi,
        c.term_exact                           AS c_term_exact,
        c.map_exact                            AS c_map,
        c.file_id                              AS file_id,
        c.line                                 AS line,
        c.col                                  AS col,
        t.id                                   AS term_id,
        t.inst_id                              AS term_inst_id,
        t.direction                            AS direction,
        mp.inner_net_id                        AS inner_net_id,
        mp.term_lo                             AS m_lo,
        mp.term_hi                             AS m_hi,
        mp.term_exact                          AS m_term_exact,
        mp.inner_lo                            AS m_net_lo,
        mp.inner_hi                            AS m_net_hi,
        mp.inner_exact                         AS m_net_exact,
        mp.map_exact                           AS m_map
    FROM net_conn c
    JOIN term t      ON t.id = c.term_id
    JOIN term_map mp ON mp.term_id = c.term_id
    LEFT JOIN hier_ref hr ON hr.id = c.outer_hier_ref_id
    WHERE c.conn_kind IN ('signal', 'expression_operand', 'constant',
                                'external_reference')
      AND (c.conn_kind != 'external_reference'
           OR hr.resolved_net_id IS NOT NULL)
      AND (c.conn_kind != 'expression_operand'
           OR c.outer_net_id IS NOT NULL OR hr.resolved_net_id IS NOT NULL)
      AND (c.term_lo IS NULL OR mp.term_hi IS NULL OR c.term_lo <= mp.term_hi)
      AND (mp.term_lo IS NULL OR c.term_hi IS NULL OR mp.term_lo <= c.term_hi)
      AND c.outer_net_id IS NULL
),
arc AS (
    SELECT seg.*,
        CASE WHEN c_lo IS NULL THEN m_lo
             WHEN m_lo IS NULL THEN c_lo
             ELSE MAX(c_lo, m_lo) END AS ilo,
        CASE WHEN c_hi IS NULL THEN m_hi
             WHEN m_hi IS NULL THEN c_hi
             ELSE MIN(c_hi, m_hi) END AS ihi,
        (COALESCE(c_map, 0) = 1 AND COALESCE(c_term_exact, 0) = 1
             AND m_term_exact = 1)    AS outer_chain,
        (m_map = 1 AND COALESCE(c_term_exact, 0) = 1
             AND m_term_exact = 1)    AS inner_chain,
        (m_lo IS NOT NULL OR m_hi IS NOT NULL) AS m_narrows,
        (c_lo IS NOT NULL OR c_hi IS NOT NULL) AS c_narrows
    FROM seg
)
SELECT
    conn_id, conn_kind, term_id, term_inst_id, direction,
    outer_net_id, inner_net_id, outer_ref,
    CASE WHEN outer_net_id IS NULL THEN NULL
         WHEN outer_chain AND c_net_exact = 1 AND ilo IS NOT NULL
              THEN COALESCE(c_net_lo, 0) + ilo - COALESCE(c_lo, 0)
         ELSE c_net_lo END          AS outer_lo,
    CASE WHEN outer_net_id IS NULL THEN NULL
         WHEN outer_chain AND c_net_exact = 1 AND ihi IS NOT NULL
              THEN COALESCE(c_net_lo, 0) + ihi - COALESCE(c_lo, 0)
         ELSE c_net_hi END          AS outer_hi,
    CASE WHEN outer_net_id IS NULL THEN NULL
         WHEN outer_chain OR NOT m_narrows THEN c_net_exact
         ELSE 0 END                 AS outer_exact,
    CASE WHEN inner_chain AND m_net_exact = 1 AND ilo IS NOT NULL
              THEN COALESCE(m_net_lo, 0) + ilo - COALESCE(m_lo, 0)
         ELSE m_net_lo END          AS inner_lo,
    CASE WHEN inner_chain AND m_net_exact = 1 AND ihi IS NOT NULL
              THEN COALESCE(m_net_lo, 0) + ihi - COALESCE(m_lo, 0)
         ELSE m_net_hi END          AS inner_hi,
    CASE WHEN inner_chain OR NOT c_narrows THEN m_net_exact
         ELSE 0 END                 AS inner_exact,
    CASE WHEN outer_net_id IS NULL THEN NULL
         WHEN conn_kind = 'expression_operand'
              THEN 0
         ELSE (c_map AND m_map) END AS map_exact,
    file_id, line, col
FROM arc;

-- Every direct driving arc of signal_net, one row each. Branches, told
-- apart by driver_kind:
--
--   data / control / primitive / procedure   a net_dep row, kind carried
--                       through; a data row with no source is 'constant'.
--   connection          the hierarchy crossing: for input/inout/ref, the
--                       parent-side net drives the child's internal net;
--                       for output/inout/ref, the internal net drives the
--                       parent's. inout and ref arc both ways, one row
--                       each. An external tie whose reference resolved
--                       crosses the same way, its far net as the outer
--                       end. Interface bindings do not arc.
--   connection_expression  the actual is an expression; each net it reads
--                       drives the internal net at range granularity.
--   constant            a tie-off or a constant RHS: driver_net_id NULL,
--                       and every driver_* column NULL with it.
--   external            the source is a reference this export has no net
--                       row for -- a package variable, an upward name from
--                       a shared body. The signal IS driven; driver_net_id
--                       is NULL because the driver has no row, not because
--                       it does not exist, and src_hier_ref_id on the
--                       dependency names the reference (path, location,
--                       resolution NULL). The driver window survives: the
--                       referenced object's bits are real even when
--                       unnamed here. Kept apart from 'constant' so
--                       "tied off" and "driven from outside the model"
--                       stay different answers -- the silence this kind
--                       replaces reported such targets as undriven.
--   alias               an alias statement binds the two nets into one
--                       object. It has no direction, so both nets appear
--                       as each other's driver and each other's load; the
--                       kind is what keeps it out of a multiple-driver
--                       count, where it would otherwise look like a second
--                       assignment.
--   system_task         a system task wrote the argument: $readmemh into a
--                       memory, $sscanf or $value$plusargs into a variable,
--                       $cast into its destination. The signal IS driven --
--                       reporting no driver said the design never wrote it
--                       -- but the source is a file or a plusarg, outside
--                       anything this schema names, so driver_net_id is
--                       NULL like a constant's and the kind keeps the two
--                       apart. stmt_id names the call.
--   terminal            the design boundary: a root input/inout/ref
--                       terminal drives the net it stands for. No driver
--                       net exists -- the world outside is the driver --
--                       and term_id names the pin, so "undriven" and
--                       "reaches the boundary" stay distinct answers.
--                       Every driver_* column is NULL, map_exact with
--                       them: there is no end to describe or correspond
--                       with, and the whole point of the null-source
--                       discipline is that a range beside a driver that
--                       does not exist is a claim about nothing. The
--                       terminal's own window is in v_term_map.
--
-- An unconnected terminal contributes no row.
--
-- driver_ref is how the driving end was SPELLED when it was reached by a
-- hierarchical name, and NULL when it was not -- one meaning wherever the
-- far end has a spelling. On a dependency it is that dependency's
-- src_hier_ref_id path: for a resolved reference (`assign q = u.x`) beside
-- the net it landed on, and for an 'external' one instead of a net, where
-- it is the ONLY name there is and the row was not self-sufficient without
-- it -- the path lived on a hier_ref the consumer had to reach through
-- dep_id, outside the views that were the whole contract. On a crossing
-- whose tie resolved (`.p(u.g[7:4])`) it is what the instantiation wrote,
-- and only on the arc whose DRIVER is that resolved net: the other
-- direction of the same tie names it as the signal, not the driver.
-- v_hier_ref holds the rest of the reference -- access, resolution, window,
-- location -- for the questions one denormalised column should not answer.
CREATE VIEW v_driver AS
SELECT
    d.tgt_net_id             AS signal_net_id,
    tn.inst_id               AS signal_inst_id,
    tn.name                  AS signal_name,
    d.tgt_lo                 AS signal_lo,
    d.tgt_hi                 AS signal_hi,
    d.tgt_exact              AS signal_exact,
    d.src_net_id             AS driver_net_id,
    sn.inst_id               AS driver_inst_id,
    sn.name                  AS driver_name,
    sh.path                  AS driver_ref,
    d.src_lo                 AS driver_lo,
    d.src_hi                 AS driver_hi,
    d.src_exact              AS driver_exact,
    -- `constant` is claimed only for a DATA row with no source, which is what
    -- the contract above says and what the word means: a tie-off or a constant
    -- right-hand side. Every other kind is carried through even without a
    -- source, because for those a missing source is not a constant -- a
    -- `pullup` has no input terminal and a call into a subroutine this
    -- template cannot name has no nameable driver, and reporting either as a
    -- tie-off inflates every multiple-driver count with a conflict that is
    -- not one. The ELSE used to swallow them.
    CASE WHEN d.src_net_id IS NOT NULL THEN d.dep_kind
         WHEN d.src_hier_ref_id IS NOT NULL THEN 'external'
         WHEN s.stmt_kind = 'system_task' THEN 'system_task'
         WHEN d.dep_kind != 'data' THEN d.dep_kind
         ELSE 'constant' END AS driver_kind,
    d.id                     AS dep_id,
    NULL                     AS conn_id,
    d.stmt_id                AS stmt_id,
    d.prim_id                AS prim_id,
    NULL                     AS term_id,
    d.map_exact              AS map_exact,
    d.call_site_id           AS call_site_id,
    f.path                   AS file_path,
    sf.path                  AS src_path,
    COALESCE(s.line, p.line) AS src_line,
    COALESCE(s.col, p.col)   AS src_col
FROM net_dep d
JOIN net tn              ON tn.id = d.tgt_net_id
LEFT JOIN net sn         ON sn.id = d.src_net_id
LEFT JOIN hier_ref sh    ON sh.id = d.src_hier_ref_id
LEFT JOIN stmt s         ON s.id = d.stmt_id
LEFT JOIN prim p    ON p.id = d.prim_id
LEFT JOIN file f         ON f.id = COALESCE(s.file_id, p.file_id)
LEFT JOIN src_file sf ON sf.id = f.src_file_id
UNION ALL
SELECT
    a.inner_net_id, innet.inst_id, innet.name,
    a.inner_lo, a.inner_hi, a.inner_exact,
    a.outer_net_id, outnet.inst_id, outnet.name, a.outer_ref,
    a.outer_lo, a.outer_hi, a.outer_exact,
    CASE a.conn_kind
         WHEN 'expression_operand' THEN 'connection_expression'
         WHEN 'constant'           THEN 'constant'
         ELSE 'connection' END,
    NULL, a.conn_id, NULL, NULL, NULL,
    a.map_exact, NULL,
    f.path, sf.path, a.line, a.col
FROM v_conn_arc a
JOIN net innet           ON innet.id = a.inner_net_id
LEFT JOIN net outnet     ON outnet.id = a.outer_net_id
LEFT JOIN file f         ON f.id = a.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id
WHERE a.direction IN ('input', 'inout', 'ref')
UNION ALL
SELECT
    a.outer_net_id, outnet.inst_id, outnet.name,
    a.outer_lo, a.outer_hi, a.outer_exact,
    a.inner_net_id, innet.inst_id, innet.name, NULL,
    a.inner_lo, a.inner_hi, a.inner_exact,
    'connection',
    NULL, a.conn_id, NULL, NULL, NULL,
    a.map_exact, NULL,
    f.path, sf.path, a.line, a.col
FROM v_conn_arc a
JOIN net innet           ON innet.id = a.inner_net_id
JOIN net outnet          ON outnet.id = a.outer_net_id
LEFT JOIN file f         ON f.id = a.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id
WHERE a.direction IN ('output', 'inout', 'ref')
  AND a.conn_kind IN ('signal', 'external_reference')
UNION ALL
SELECT
    m.inner_net_id, n.inst_id, n.name,
    m.inner_lo, m.inner_hi, m.inner_exact,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    'terminal',
    NULL, NULL, NULL, NULL, t.id,
    NULL, NULL,
    f.path, sf.path, t.line, t.col
FROM term_map m
JOIN term t              ON t.id = m.term_id
JOIN tree_node r         ON r.id = t.inst_id AND r.node_kind = 'root'
JOIN net n               ON n.id = m.inner_net_id
LEFT JOIN file f         ON f.id = t.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id
WHERE t.direction IN ('input', 'inout', 'ref');

-- Every recorded read of signal_net, one row each -- v9's generalised load
-- semantics carried to the instance level, plus the crossing. load_kind:
--
--   dataflow      a net_dep row: the signal feeds a target (data, control,
--                 primitive or procedure -- join net_dep on dep_id
--                 for which).
--   connection    the crossing reads it: a parent net feeding an
--                 input/inout/ref terminal, an internal net feeding an
--                 output/inout/ref one, or a resolved external tie either
--                 way. load_net is the far side.
--   sensitivity   a procedure triggers on it (proc_event, or an expr_ref
--                 with role='event' when the expression was not plain).
--   wait          a procedure suspends on it (proc_event, or role='wait').
--   statement     a statement reads it and writes nothing this instance
--                 names: an assertion, a $display, a call argument or
--                 branch condition whose statement has no local target.
--                 load_* are all NULL -- a real reader, no nameable target.
--   terminal      the design boundary: a root output/inout/ref terminal
--                 reads the net it stands for. load_* are NULL and
--                 term_id names the pin, so "unused" and "reaches the
--                 boundary" stay distinct answers.
--
-- One read, one row: an expr_ref or assign_operand that a net_dep row
-- already carries into 'dataflow' is not repeated as 'statement' -- the
-- NOT EXISTS guards are that rule, and the verifier re-derives the counts.
--
-- load_ref is v_driver's driver_ref with the ends exchanged: how the LOAD
-- end was spelled when it was reached by a hierarchical name, NULL when it
-- was not. A dataflow row takes it from the dependency's target reference
-- (`tb.glob = sig` reads sig into a name this instance does not own), a
-- crossing from the resolved tie on whichever side is the load. The
-- 'statement' branch never carries one: a read whose statement writes
-- outward has as many outward targets as the statement has, and pairing the
-- read with them is the cross product that would cost this view its one
-- row per read.
CREATE VIEW v_load AS
SELECT
    d.src_net_id             AS signal_net_id,
    sn.inst_id               AS signal_inst_id,
    sn.name                  AS signal_name,
    d.src_lo                 AS signal_lo,
    d.src_hi                 AS signal_hi,
    d.src_exact              AS signal_exact,
    d.tgt_net_id             AS load_net_id,
    tn.inst_id               AS load_inst_id,
    tn.name                  AS load_name,
    th.path                  AS load_ref,
    d.tgt_lo                 AS load_lo,
    d.tgt_hi                 AS load_hi,
    d.tgt_exact              AS load_exact,
    CASE WHEN d.dep_kind = 'alias' THEN 'alias'
         ELSE 'dataflow' END AS load_kind,
    d.id                     AS dep_id,
    NULL                     AS conn_id,
    d.stmt_id                AS stmt_id,
    s.proc_id                AS proc_id,
    NULL                     AS term_id,
    d.map_exact              AS map_exact,
    d.call_site_id           AS call_site_id,
    f.path                   AS file_path,
    sf.path                  AS src_path,
    COALESCE(s.line, p.line) AS src_line,
    COALESCE(s.col, p.col)   AS src_col
FROM net_dep d
JOIN net sn              ON sn.id = d.src_net_id
JOIN net tn              ON tn.id = d.tgt_net_id
LEFT JOIN hier_ref th    ON th.id = d.tgt_hier_ref_id
LEFT JOIN stmt s         ON s.id = d.stmt_id
LEFT JOIN prim p    ON p.id = d.prim_id
LEFT JOIN file f         ON f.id = COALESCE(s.file_id, p.file_id)
LEFT JOIN src_file sf ON sf.id = f.src_file_id
UNION ALL
SELECT
    a.outer_net_id, outnet.inst_id, outnet.name,
    a.outer_lo, a.outer_hi, a.outer_exact,
    a.inner_net_id, innet.inst_id, innet.name, NULL,
    a.inner_lo, a.inner_hi, a.inner_exact,
    'connection',
    NULL, a.conn_id, NULL, NULL, NULL,
    a.map_exact, NULL,
    f.path, sf.path, a.line, a.col
FROM v_conn_arc a
JOIN net outnet          ON outnet.id = a.outer_net_id
JOIN net innet           ON innet.id = a.inner_net_id
LEFT JOIN file f         ON f.id = a.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id
WHERE a.direction IN ('input', 'inout', 'ref')
UNION ALL
SELECT
    a.inner_net_id, innet.inst_id, innet.name,
    a.inner_lo, a.inner_hi, a.inner_exact,
    a.outer_net_id, outnet.inst_id, outnet.name, a.outer_ref,
    a.outer_lo, a.outer_hi, a.outer_exact,
    'connection',
    NULL, a.conn_id, NULL, NULL, NULL,
    a.map_exact, NULL,
    f.path, sf.path, a.line, a.col
FROM v_conn_arc a
JOIN net innet           ON innet.id = a.inner_net_id
JOIN net outnet          ON outnet.id = a.outer_net_id
LEFT JOIN file f         ON f.id = a.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id
WHERE a.direction IN ('output', 'inout', 'ref')
  AND a.conn_kind IN ('signal', 'external_reference')
UNION ALL
SELECT
    pe.net_id, n.inst_id, n.name,
    NULL, NULL, 1,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    pe.event_kind,
    NULL, NULL, pe.stmt_id, pe.proc_id, NULL, NULL, NULL,
    f.path, sf.path, pe.line, pe.col
FROM proc_event pe
JOIN net n               ON n.id = pe.net_id
LEFT JOIN file f         ON f.id = pe.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id
UNION ALL
SELECT
    e.net_id, n.inst_id, n.name,
    e.lo, e.hi, e.is_exact,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    CASE e.role WHEN 'wait' THEN 'wait'
                WHEN 'event' THEN 'sensitivity'
                ELSE 'statement' END,
    NULL, NULL, e.stmt_id, s.proc_id, NULL, NULL, NULL,
    f.path, sf.path, s.line, s.col
FROM expr_ref e
JOIN net n               ON n.id = e.net_id
JOIN stmt s              ON s.id = e.stmt_id
LEFT JOIN file f         ON f.id = s.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id
WHERE e.role IN ('assertion', 'wait', 'event', 'system_task')
   OR NOT EXISTS (SELECT 1 FROM net_dep d WHERE d.expr_ref_id = e.id)
UNION ALL
SELECT
    o.net_id, n.inst_id, n.name,
    o.lo, o.hi, o.is_exact,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    'statement',
    NULL, NULL, o.stmt_id, s.proc_id, NULL, NULL, NULL,
    f.path, sf.path, s.line, s.col
FROM assign_operand o
JOIN net n               ON n.id = o.net_id
JOIN stmt s              ON s.id = o.stmt_id
LEFT JOIN file f         ON f.id = s.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id
WHERE NOT EXISTS (SELECT 1 FROM net_dep d WHERE d.assign_operand_id = o.id)
UNION ALL
SELECT
    m.inner_net_id, n.inst_id, n.name,
    m.inner_lo, m.inner_hi, m.inner_exact,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    'terminal',
    NULL, NULL, NULL, NULL, t.id, NULL, NULL,
    f.path, sf.path, t.line, t.col
FROM term_map m
JOIN term t              ON t.id = m.term_id
JOIN tree_node r         ON r.id = t.inst_id AND r.node_kind = 'root'
JOIN net n               ON n.id = m.inner_net_id
LEFT JOIN file f         ON f.id = t.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id
WHERE t.direction IN ('output', 'inout', 'ref');

-- One row per stmt: the statement object. Targets and operands are
-- v_stmt_target / v_stmt_operand rows keyed on stmt_id --
-- one statement is ONE row here no matter how many targets it writes.
--
-- call_site_id is the body expansion this statement was walked for, the
-- same tag v_net_dep and the two directional views carry. It was on the
-- base table from v13 and missing here, which left the documented
-- context-sensitive recipe -- "follow only the rows of one call's site" --
-- unexecutable for a walk that starts at a statement rather than at a
-- dependency.
CREATE VIEW v_stmt AS
SELECT
    s.id                    AS stmt_id,
    s.inst_id               AS inst_id,
    i.module_id             AS module_id,
    m.name                  AS module_name,
    s.scope_node_id         AS scope_node_id,
    s.proc_id               AS proc_id,
    s.ordinal               AS ordinal,
    s.sequence              AS sequence,
    s.stmt_kind             AS stmt_kind,
    s.construct             AS construct,
    s.assign_kind           AS assign_kind,
    s.delay                 AS delay,
    s.dropped_operand_count AS dropped_operand_count,
    s.call_site_id          AS call_site_id,
    f.path                  AS file_path,
    sf.path                 AS src_path,
    s.line                  AS src_line,
    s.col                   AS src_col
FROM stmt s
JOIN inst i              ON i.id = s.inst_id
LEFT JOIN module m       ON m.id = i.module_id
LEFT JOIN file f         ON f.id = s.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id;

-- One row per stmt_target -- an lvalue, which is not the same thing as a
-- write. target_kind says which, in the vocabulary v_net_attachment already
-- contracts for the same three storage cases:
--
--   written_by       an assignment or a system task writes the net.
--   release_target   a `release`/`deassign` names its lvalue and drives
--                    NOTHING; the extractor gives it a target row and
--                    deliberately no dependency, so this table is the only
--                    place it appears -- and, without this column, the only
--                    way to mistake it for a driver.
--   alias_binding    an `alias` statement binds the net into one object
--                    with another. Same storage, no write; v_driver keeps
--                    it out of a driver count by kind, and this does the
--                    same for a consumer starting from the statement side.
--
-- The CASE is v_net_attachment's, character for character; the verifier
-- holds the two equal per target row, so the repetition cannot drift. It
-- costs a join against stmt's primary key and no fan-out. v_stmt_operand
-- has no counterpart: a read is a read whatever statement makes it.
--
-- call_site_id is the statement's, carried here so the row answers on its
-- own which expansion wrote this net. The statement layer is where the
-- question bites hardest -- one body walked for N calls is N sets of these
-- rows over one set of formal nets.
CREATE VIEW v_stmt_target AS
SELECT
    a.id       AS target_id,
    a.stmt_id  AS stmt_id,
    a.ordinal  AS ordinal,
    a.net_id   AS net_id,
    n.name     AS net_name,
    CASE WHEN s.stmt_kind = 'release' THEN 'release_target'
         WHEN s.stmt_kind = 'alias'   THEN 'alias_binding'
         ELSE 'written_by' END AS target_kind,
    a.lo       AS tgt_lo,
    a.hi       AS tgt_hi,
    a.is_exact AS tgt_exact,
    s.call_site_id AS call_site_id
FROM stmt_target a
JOIN net n ON n.id = a.net_id
JOIN stmt s ON s.id = a.stmt_id;

-- One row per assign_operand. call_site_id is its statement's, for the
-- reason v_stmt carries one: a body called from N sites is N sets of these
-- rows over one set of formal nets, and without the tag on the row itself a
-- consumer starting here cannot tell the sets apart -- it would have to
-- rejoin v_stmt for a column that statement already knows.
CREATE VIEW v_stmt_operand AS
SELECT
    o.id       AS operand_id,
    o.stmt_id  AS stmt_id,
    o.ordinal  AS ordinal,
    o.net_id   AS net_id,
    n.name     AS net_name,
    o.lo       AS operand_lo,
    o.hi       AS operand_hi,
    o.is_exact AS operand_exact,
    s.call_site_id AS call_site_id
FROM assign_operand o
JOIN net n ON n.id = o.net_id
JOIN stmt s ON s.id = o.stmt_id;

-- Everything touching one net, one row per attachment: the structural
-- adjacency that v_driver/v_load answer directionally, answered flatly --
-- "what hangs off this net". attachment_kind names the relation, and
-- exactly ONE of the seven typed id columns points at that relation's own
-- row -- the same exclusive-arc shape net_dep uses, rather than one
-- polymorphic id whose meaning a consumer had to reconstruct from the
-- kind. The typed column each kind fills:
--
--   terminal_inside     term_id      term_map: the net stands behind a
--                                    terminal of its own instance.
--   actual_outside      term_id      net_conn: the net is wired to a
--                                    child's terminal, named directly
--                                    (outer_net_id) or through a resolved
--                                    external tie (`.p(u.g[7:4])`).
--   written_by          stmt_target_id   stmt_target of an assignment or
--                                    system-task write.
--   release_target      stmt_target_id   stmt_target of a
--                                    `release`/`deassign`. Same storage,
--                                    but the statement drives NOTHING --
--                                    a separate kind so "what writes this
--                                    net" excludes it, as no net_dep does.
--   read_by             assign_operand_id
--   condition           expr_ref_id  expr_ref with role='control'.
--   statement_read      expr_ref_id  every other expr_ref role.
--   event               proc_id      proc_event: a procedure triggers or
--                                    waits on it. proc_id is the procedure.
--   dep_in / dep_out    dep_id       net_dep, by end.
--   named_from_outside  hier_ref_id  a resolved hier_ref lands on it.
--
-- lo/hi/exact are THIS net's window in the attachment (whole, exact, for
-- an event); stmt_id is set where a statement exists. Each branch is one
-- base selection, reconcilable by count; a point query by net_id seeks on
-- every branch; and the verifier holds that exactly one typed id is
-- non-null per row and is the one attachment_kind implies.
CREATE VIEW v_net_attachment AS
SELECT m.inner_net_id AS net_id, n.inst_id AS inst_id, n.name AS net_name,
       'terminal_inside' AS attachment_kind,
       m.inner_lo AS lo, m.inner_hi AS hi, m.inner_exact AS exact,
       NULL AS stmt_id,
       m.term_id AS term_id, NULL AS stmt_target_id, NULL AS assign_operand_id,
       NULL AS expr_ref_id, NULL AS proc_id, NULL AS dep_id,
       NULL AS hier_ref_id
FROM term_map m JOIN net n ON n.id = m.inner_net_id
UNION ALL
SELECT c.outer_net_id, n.inst_id, n.name, 'actual_outside',
       c.outer_lo, c.outer_hi, c.outer_exact, NULL,
       c.term_id, NULL, NULL, NULL, NULL, NULL, NULL
FROM net_conn c JOIN net n ON n.id = c.outer_net_id
WHERE c.outer_net_id IS NOT NULL
UNION ALL
-- The far side of a resolved external tie: the same wiring fact as a plain
-- signal connection, but the net lives in another instance and the window
-- lives on the hier_ref. Without this branch the crossing a `.p(u.g[7:4])`
-- makes showed up only as named_from_outside, and "which child pin does
-- this net feed" answered nothing for exactly the ties v12 taught to arc.
SELECT h.resolved_net_id, n.inst_id, n.name, 'actual_outside',
       h.lo, h.hi, h.is_exact, NULL,
       c.term_id, NULL, NULL, NULL, NULL, NULL, NULL
FROM net_conn c
JOIN hier_ref h ON h.id = c.outer_hier_ref_id
JOIN net n ON n.id = h.resolved_net_id
WHERE h.resolved_net_id IS NOT NULL
UNION ALL
-- `alias_binding` is its own kind for the reason `release_target` is: the
-- storage is a stmt_target row, and the statement does not WRITE the net.
-- An alias has no direction and no driver -- v_driver keeps it out of the
-- driver count by giving it driver_kind='alias', and the doc's rule that
-- "kinds keep counts right" only held on that side. Here every alias side
-- was reported as written, so the same database answered "who writes this
-- net" two ways, and named nets that no assignment in the design touches.
SELECT a.net_id, n.inst_id, n.name,
       CASE WHEN s.stmt_kind = 'release' THEN 'release_target'
            WHEN s.stmt_kind = 'alias'   THEN 'alias_binding'
            ELSE 'written_by' END,
       a.lo, a.hi, a.is_exact, a.stmt_id,
       NULL, a.id, NULL, NULL, NULL, NULL, NULL
FROM stmt_target a JOIN net n ON n.id = a.net_id
JOIN stmt s ON s.id = a.stmt_id
UNION ALL
SELECT o.net_id, n.inst_id, n.name, 'read_by',
       o.lo, o.hi, o.is_exact, o.stmt_id,
       NULL, NULL, o.id, NULL, NULL, NULL, NULL
FROM assign_operand o JOIN net n ON n.id = o.net_id
UNION ALL
SELECT e.net_id, n.inst_id, n.name,
       CASE WHEN e.role = 'control' THEN 'condition'
            ELSE 'statement_read' END,
       e.lo, e.hi, e.is_exact, e.stmt_id,
       NULL, NULL, NULL, e.id, NULL, NULL, NULL
FROM expr_ref e JOIN net n ON n.id = e.net_id
UNION ALL
SELECT pe.net_id, n.inst_id, n.name, 'event',
       NULL, NULL, 1, pe.stmt_id,
       NULL, NULL, NULL, NULL, pe.proc_id, NULL, NULL
FROM proc_event pe JOIN net n ON n.id = pe.net_id
WHERE pe.net_id IS NOT NULL
UNION ALL
SELECT d.tgt_net_id, n.inst_id, n.name, 'dep_in',
       d.tgt_lo, d.tgt_hi, d.tgt_exact, d.stmt_id,
       NULL, NULL, NULL, NULL, NULL, d.id, NULL
FROM net_dep d JOIN net n ON n.id = d.tgt_net_id
UNION ALL
SELECT d.src_net_id, n.inst_id, n.name, 'dep_out',
       d.src_lo, d.src_hi, d.src_exact, d.stmt_id,
       NULL, NULL, NULL, NULL, NULL, d.id, NULL
FROM net_dep d JOIN net n ON n.id = d.src_net_id
WHERE d.src_net_id IS NOT NULL
UNION ALL
SELECT h.resolved_net_id, n.inst_id, n.name, 'named_from_outside',
       h.lo, h.hi, h.is_exact, h.stmt_id,
       NULL, NULL, NULL, NULL, NULL, NULL, h.id
FROM hier_ref h JOIN net n ON n.id = h.resolved_net_id
WHERE h.resolved_net_id IS NOT NULL;

-- One row per call_site: a subroutine-body expansion, the context a
-- net_dep or stmt names in its call_site_id. Walk parent_call_site_id to
-- recover the call string; caller_stmt_id is the statement that made the
-- call. depth is 1 at the outermost call. A fan-in cone stays
-- context-sensitive by following, at each hop through a subroutine, only
-- the rows whose call_site_id shares the chain -- never mixing one call's
-- gating with another's argument, the combination the shared formal admits.
CREATE VIEW v_call_site AS
SELECT
    cs.id                  AS call_site_id,
    cs.inst_id             AS inst_id,
    i.module_id            AS module_id,
    m.name                 AS module_name,
    cs.caller_stmt_id      AS caller_stmt_id,
    cs.parent_call_site_id AS parent_call_site_id,
    cs.subroutine_name     AS subroutine_name,
    cs.depth               AS depth
FROM call_site cs
JOIN inst i        ON i.id = cs.inst_id
LEFT JOIN module m ON m.id = i.module_id;

-- One row per hier_ref: a reference that leaves its instance, as written
-- and -- where slang could settle it and the target is in this export --
-- as resolved.
--
-- The contract handed consumers four pointers at this table before it
-- handed them a way to follow one: v_net_dep's src_hier_ref_id and
-- tgt_hier_ref_id, v_net_conn's outer_hier_ref_id, v_net_attachment's
-- hier_ref_id. A published foreign key with nothing to resolve it against
-- is a dangling reference in the contract itself, and the consumer that
-- needed the path read the base table -- correctly, and outside every
-- promise this schema makes.
--
-- driver_ref/load_ref answer the common case in one row, so a walk over
-- either directional view never needs this. What is here is the rest of the
-- reference, which no denormalised column should carry: access, where it
-- landed, its window, its location.
--
-- ref_path rather than path: this view spells three different things
-- 'path' -- the reference, the file as the filelist wrote it, and the file
-- slang read -- and only prefixes keep them apart.
CREATE VIEW v_hier_ref AS
SELECT
    h.id                AS hier_ref_id,
    h.inst_id           AS inst_id,
    i.module_id         AS module_id,
    m.name              AS module_name,
    h.stmt_id           AS stmt_id,
    h.path              AS ref_path,
    h.access            AS access,
    h.resolved_inst_id  AS resolved_inst_id,
    h.resolved_net_id   AS resolved_net_id,
    rn.name             AS resolved_net_name,
    h.lo                AS ref_lo,
    h.hi                AS ref_hi,
    h.is_exact          AS ref_exact,
    f.path              AS file_path,
    sf.path             AS src_path,
    h.line              AS src_line,
    h.col               AS src_col
FROM hier_ref h
JOIN inst i           ON i.id = h.inst_id
LEFT JOIN module m    ON m.id = i.module_id
LEFT JOIN net rn      ON rn.id = h.resolved_net_id
LEFT JOIN file f      ON f.id = h.file_id
LEFT JOIN src_file sf ON sf.id = f.src_file_id;
)SQL"
