// The first two trees of the pipeline: `original`, every file the document is
// built from keyed by its path, and `combined`, the top-level file with its
// includes spliced in and its `load`ed files merged. See pals_pipeline.h.

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <ryml.hpp>
#include <ryml_std.hpp>

#include "PALSParserCpp.h"
#include "pals_pipeline.h"
#include "pals_util.h"
#include "yaml_tree.h"

namespace pals {

// ============================================================
// FILE REFERENCE PATHS (include / load)
// ============================================================
//
// A file named by an `include` or a `load` is located relative to the file that
// names it, not to the process working directory, so a set of files that refer
// to each other can be moved or checked out anywhere as a unit. Resolution is
// purely lexical -- the naming file's directory is prepended and the result is
// folded -- so one file reaches the `original` master tree under one key
// whatever route led to it, and a reference cycle is recognisable by that key.

// Everything up to the last '/' in `p`; empty when `p` names a bare file.
static std::string path_dir(const std::string& p) {
    size_t slash = p.find_last_of('/');
    return (slash == std::string::npos) ? std::string() : p.substr(0, slash);
}

// Collapse "." and "x/.." segments and repeated separators. Leading ".." is
// kept (there is no way to know what it resolves to without touching the
// filesystem) except on an absolute path, where it cannot rise above "/".
std::string fold_path(const std::string& p) {
    const bool absolute = !p.empty() && p[0] == '/';
    std::vector<std::string> parts;
    for (size_t i = 0; i < p.size();) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string seg = p.substr(i, j - i);
        i = j + 1;
        if (seg.empty() || seg == ".") continue;
        if (seg != "..") {
            parts.push_back(seg);
        } else if (!parts.empty() && parts.back() != "..") {
            parts.pop_back();
        } else if (!absolute) {
            parts.push_back(seg);
        }
    }
    std::string out = absolute ? "/" : "";
    for (size_t k = 0; k < parts.size(); ++k) {
        if (k) out += "/";
        out += parts[k];
    }
    if (out.empty()) out = ".";
    return out;
}

// The path of the file `ref` names, as named from inside the file at `from`.
static std::string resolve_path(const std::string& from,
                                const std::string& ref) {
    if (!ref.empty() && ref[0] == '/') return fold_path(ref);
    std::string dir = path_dir(from);
    return fold_path(dir.empty() ? ref : dir + "/" + ref);
}

/**
 * Recursive helper for make_combined_from_original. Starting from `node` in the
 * combined tree `t`, replace every "include: filename" element with the
 * contents of that file, sourced from the already-parsed `original` tree so
 * provenance can be recorded. Also recurses into spliced content to handle
 * nested include statements.
 *
 * `path` is the resolved path of the file whose contents `node` sits in, which
 * is what its include references are relative to. Spliced-in content is walked
 * with the included file's own path, so a chain of includes each resolves
 * against the file that wrote it.
 */
static void make_combined_splice(ryml::Tree& t, size_t node, ParsedData* orig,
                                 const std::string& path,
                                 Provenance& prov) {
    if (node == ryml::NONE) return;

    if (t.is_seq(node)) {
        size_t child = t.first_child(node);
        while (child != ryml::NONE) {
            size_t next = t.next_sibling(child);

            // Seq elements are anonymous MAP wrappers, so - include: "file"
            // looks like:
            //   anon wrapper (MAP, no key) -> KEYVAL(key="include", val="file")
            std::string filename;
            bool is_include = false;
            if (!t.has_key(child) && t.is_map(child) &&
                t.num_children(child) == 1) {
                size_t inner = t.first_child(child);
                if (inner != ryml::NONE && t.has_key(inner) &&
                    t.key(inner) == ryml::to_csubstr("include") &&
                    t.has_val(inner)) {
                    is_include = true;
                    filename = val_str(t, inner);
                }
            }

            if (is_include) {
                // Look up the included file's raw contents in `original`. It is
                // stored keyed by its path resolved against the naming file --
                // the same key make_original registered it under.
                const std::string inc_path = resolve_path(path, filename);
                ryml::Tree& ot = orig->tree;
                size_t inc_root =
                    ot.find_child(ot.root_id(), ryml::to_csubstr(inc_path));
                size_t after = child;
                std::vector<size_t> inserted;
                if (inc_root != ryml::NONE) {
                    // Splice each child of the included file's root into this
                    // sequence, recording provenance into `original`.
                    if (ot.is_seq(inc_root)) {
                        for (size_t c = ot.first_child(inc_root);
                             c != ryml::NONE; c = ot.next_sibling(c)) {
                            ensure_capacity(t);
                            size_t n = t.insert_child(node, after);
                            deep_copy_tracked(t, n, ot, c, prov);
                            inserted.push_back(n);
                            after = n;
                        }
                    } else {
                        // Included root is not a sequence — insert as a single
                        // element
                        ensure_capacity(t);
                        size_t n = t.insert_child(node, after);
                        deep_copy_tracked(t, n, ot, inc_root, prov);
                        inserted.push_back(n);
                    }
                }
                // Recurse into inserted nodes to handle nested includes. Those
                // are the included file's own, so they resolve against it.
                for (size_t n : inserted)
                    make_combined_splice(t, n, orig, inc_path, prov);
                remove_tracked(t, child, prov);
                child = next;
                continue;
            }

            make_combined_splice(t, child, orig, path, prov);
            child = next;
        }
        return;
    }

    for (size_t c = t.first_child(node); c != ryml::NONE; c = t.next_sibling(c))
        make_combined_splice(t, c, orig, path, prov);
}

// ============================================================
// LOAD
// ============================================================
//
// `load` and `include` both draw several files into the combined tree, but
// where an include splices a file's contents in verbatim at the point it is
// written, a load merges whole files subnode by subnode under the `PALS` root.
// The typical use is a layout file plus one of several settings files, joined
// by a small file that names both.
//
// Includes are resolved first, so each file is complete before files are
// combined; loading is bottom-up, so a loaded file that loads further files is
// itself a single merged file by the time it is merged into its joiner.

// The `load` list entry standing for the joiner file's own contents. Absent
// from the list, they are merged last.
static const char* const LOAD_SELF = "SELF";

// Structural equality of two subtrees: same shape, keys and values throughout.
// `load` allows two files to supply the same dictionary entry only when they
// agree on it, and this is what "agree" means.
static bool nodes_equal(const ryml::Tree& t, size_t a, size_t b) {
    if (a == ryml::NONE || b == ryml::NONE) return a == b;
    if (t.is_map(a) != t.is_map(b) || t.is_seq(a) != t.is_seq(b)) return false;
    if (t.has_key(a) != t.has_key(b)) return false;
    if (t.has_key(a) && t.key(a) != t.key(b)) return false;
    if (t.has_val(a) != t.has_val(b)) return false;
    if (t.has_val(a) && t.val(a) != t.val(b)) return false;
    size_t ca = t.first_child(a), cb = t.first_child(b);
    for (; ca != ryml::NONE && cb != ryml::NONE;
         ca = t.next_sibling(ca), cb = t.next_sibling(cb))
        if (!nodes_equal(t, ca, cb)) return false;
    return ca == ryml::NONE && cb == ryml::NONE;
}

// Combine two `version` strings. Files written against different versions of
// the schema keep both on record rather than one silently winning, so the
// combined value is the distinct versions in first-seen order, comma delimited.
// `have` may already be such a list from an earlier merge.
static std::string merge_versions(const std::string& have,
                                  const std::string& add) {
    std::vector<std::string> seen;
    auto note = [&seen](const std::string& raw) {
        std::string v = trim(raw, " \t");
        if (v.empty()) return;
        if (std::find(seen.begin(), seen.end(), v) == seen.end())
            seen.push_back(v);
    };
    for (size_t i = 0; i <= have.size();) {
        size_t j = have.find(',', i);
        if (j == std::string::npos) j = have.size();
        note(have.substr(i, j - i));
        i = j + 1;
    }
    note(add);
    std::string out;
    for (size_t k = 0; k < seen.size(); ++k) {
        if (k) out += ", ";
        out += seen[k];
    }
    return out;
}

/**
 * Merge one file's `PALS` subnodes into the accumulating `dest` map.
 *
 * A subnode `dest` does not have yet is copied in whole. Otherwise the two are
 * combined by type: list subnodes (`notes`, `authors`, `facility`, ...)
 * concatenate, so the combined list keeps the order of the `load` list and,
 * within each file, the order written there; Dict subnodes
 * (`extension_labels`) take the union, with a duplicate entry discarded when
 * the two files agree on it and reported when they do not; and `version`
 * collects the distinct version strings. Any other disagreement over a plain
 * value is reported and the value already in `dest` -- the earlier file's --
 * stands.
 *
 * `src_name` names the file `src` came from, for problem messages.
 */
static void merge_pals_into(ryml::Tree& t, size_t dest, size_t src,
                            const std::string& src_name,
                            Provenance& prov,
                            ProblemList& problems) {
    if (dest == ryml::NONE || src == ryml::NONE || !t.is_map(src)) return;

    for (size_t sc = t.first_child(src); sc != ryml::NONE;
         sc = t.next_sibling(sc)) {
        if (!t.has_key(sc)) continue;
        // Held as a std::string: writing into the tree's arena below can
        // relocate it, and with it any csubstr pointing into it.
        const std::string key = key_str(t, sc);
        const size_t dc = t.find_child(dest, ryml::to_csubstr(key));

        if (dc == ryml::NONE) {
            ensure_capacity(t, 2);
            duplicate_tracked(t, sc, dest, t.last_child(dest), prov);
            continue;
        }

        if (key == "version" && t.has_val(dc) && t.has_val(sc)) {
            const std::string merged =
                merge_versions(val_str(t, dc),
                               val_str(t, sc));
            t.set_val(dc, t.to_arena(ryml::to_csubstr(merged)));
            continue;
        }

        if (t.is_seq(dc) && t.is_seq(sc)) {
            size_t after = t.last_child(dc);
            for (size_t e = t.first_child(sc); e != ryml::NONE;
                 e = t.next_sibling(e)) {
                ensure_capacity(t, 2);
                after = duplicate_tracked(t, e, dc, after, prov);
            }
            continue;
        }

        if (t.is_map(dc) && t.is_map(sc)) {
            size_t after = t.last_child(dc);
            for (size_t e = t.first_child(sc); e != ryml::NONE;
                 e = t.next_sibling(e)) {
                if (!t.has_key(e)) continue;
                const std::string sub = key_str(t, e);
                size_t have = t.find_child(dc, ryml::to_csubstr(sub));
                if (have == ryml::NONE) {
                    ensure_capacity(t, 2);
                    after = duplicate_tracked(t, e, dc, after, prov);
                } else if (!nodes_equal(t, have, e)) {
                    add_problem(problems, "load: '" + src_name +
                                              "' disagrees on the value of '" +
                                              key + "." + sub + "'");
                }
            }
            continue;
        }

        if (t.has_val(dc) && t.has_val(sc) && t.val(dc) == t.val(sc)) continue;

        add_problem(problems, "load: '" + src_name +
                                  "' disagrees on the value of '" + key + "'");
    }
}

/**
 * Resolve one file's `load` list, in place, replacing its `PALS` node with the
 * merge of every file it names.
 *
 * `pals` is the file's `PALS` node in the combined tree and `path` the resolved
 * path of the file it came from, which its own load references are relative to.
 * Each loaded file is staged under a scratch node: copied out of `original`,
 * spliced for its own includes, then resolved for its own `load` -- so it is a
 * single complete file before it takes part in this merge, and nesting works to
 * any depth. The joiner's own contents are staged the same way, which is what
 * lets `SELF` sit anywhere in the order: `pals` can then be emptied and refilled
 * with the merge of every source, taken in load-list order.
 *
 * `active` holds the files whose loads are being resolved further up the
 * recursion, so a cycle is reported rather than followed forever.
 */
static void resolve_loads(ryml::Tree& t, size_t pals, ParsedData* orig,
                          const std::string& path,
                          Provenance& prov,
                          std::set<std::string>& active,
                          ProblemList& problems) {
    if (pals == ryml::NONE || !t.is_map(pals)) return;
    size_t load = t.find_child(pals, ryml::to_csubstr("load"));
    if (load == ryml::NONE) return;

    std::vector<std::string> refs;
    bool has_self = false;
    if (t.is_seq(load)) {
        for (size_t e = t.first_child(load); e != ryml::NONE;
             e = t.next_sibling(e)) {
            if (!t.has_val(e)) continue;
            refs.push_back(val_str(t, e));
            if (refs.back() == LOAD_SELF) has_self = true;
        }
    } else if (t.has_val(load)) {
        // A lone filename, written without the list punctuation.
        refs.push_back(val_str(t, load));
    }
    if (!has_self) refs.push_back(LOAD_SELF);

    // The `load` node is a directive, not content: it takes no part in the
    // merge and must not survive into the combined tree.
    remove_tracked(t, load, prov);

    // Staging area. Keyed by the path each file came from, which makes a dumped
    // intermediate tree readable; nothing looks these keys up.
    ensure_capacity(t, 2);
    size_t scratch = t.append_child(t.root_id());
    t.to_map(scratch, t.to_arena(ryml::to_csubstr("__load")));

    // The joiner's own contents, put aside before `pals` is emptied.
    ensure_capacity(t, 2);
    size_t self_node = t.append_child(scratch);
    t.to_map(self_node, t.to_arena(ryml::to_csubstr(LOAD_SELF)));
    {
        size_t after = ryml::NONE;
        for (size_t c = t.first_child(pals); c != ryml::NONE;
             c = t.next_sibling(c)) {
            ensure_capacity(t, 2);
            after = duplicate_tracked(t, c, self_node, after, prov);
        }
    }

    std::vector<size_t> sources;     // PALS nodes to merge, in load order
    std::vector<std::string> names;  // where each came from, for messages

    ryml::Tree& ot = orig->tree;
    for (const std::string& ref : refs) {
        if (ref == LOAD_SELF) {
            sources.push_back(self_node);
            names.push_back(path);
            continue;
        }

        const std::string sub = resolve_path(path, ref);
        if (active.count(sub)) {
            add_problem(problems, "load: '" + path + "' loads '" + sub +
                                      "', which is already being loaded");
            continue;
        }
        size_t src_root = ot.find_child(ot.root_id(), ryml::to_csubstr(sub));
        if (src_root == ryml::NONE) {
            add_problem(problems, "load: could not read '" + sub +
                                      "', loaded from '" + path + "'");
            continue;
        }

        ensure_capacity(t, 2);
        size_t stage = t.append_child(scratch);
        deep_copy_tracked(t, stage, ot, src_root, prov);
        make_combined_splice(t, stage, orig, sub, prov);

        size_t sub_pals = t.find_child(stage, ryml::to_csubstr("PALS"));
        if (sub_pals == ryml::NONE) {
            add_problem(problems,
                        "load: '" + sub + "' has no PALS node to load");
            continue;
        }
        active.insert(sub);
        resolve_loads(t, sub_pals, orig, sub, prov, active, problems);
        active.erase(sub);

        sources.push_back(sub_pals);
        names.push_back(sub);
    }

    for (size_t c = t.first_child(pals); c != ryml::NONE;) {
        size_t next = t.next_sibling(c);
        remove_tracked(t, c, prov);
        c = next;
    }

    for (size_t i = 0; i < sources.size(); ++i)
        merge_pals_into(t, pals, sources[i], names[i], prov, problems);

    remove_tracked(t, scratch, prov);
}

/**
 * Makes the combined lattice tree by deep-copying the top-level file's contents
 * out of the `original` tree, splicing in every include (including nested ones)
 * from `original`, and then merging in every `load`ed file. Records provenance
 * mapping each combined node to the original node it was copied from.
 *
 * @param orig     The already-built `original` tree (see make_original).
 * @param filename The top-level file's resolved path, used to find its entry in
 *                 `original` and to resolve the references it makes.
 */
YAMLTreeHandle make_combined_from_original(ParsedData* orig,
                                           const std::string& filename,
                                           ProblemList& problems) {
    if (!orig) return nullptr;
    ParsedData* data = new ParsedData();
    ryml::Tree& t = data->tree;
    t.reserve(t.capacity() + 128);
    t.reserve_arena(t.arena_capacity() + 65536);

    ryml::Tree& ot = orig->tree;
    // The top-level file is stored in `original` keyed by its path.
    size_t top = ot.find_child(ot.root_id(), ryml::to_csubstr(filename));
    if (top == ryml::NONE) top = ot.first_child(ot.root_id());
    if (top == ryml::NONE) return data;

    deep_copy_tracked(t, t.root_id(), ot, top, data->provenance);
    make_combined_splice(t, t.root_id(), orig, filename, data->provenance);

    // Loads combine whole files, so they run once each file is itself complete.
    std::set<std::string> active{filename};
    resolve_loads(t, t.find_child(t.root_id(), ryml::to_csubstr("PALS")), orig,
                  filename, data->provenance, active, problems);
    return data;
}

/**
 * Recursive helper for make_original. Walks the contents of one already-parsed
 * file and, for every file it references -- by `include` or by `load` -- adds
 * that file to `master` under its resolved path, then walks it in turn.
 *
 * `src_path` is the resolved path of the file `src` holds, which is what its
 * references are relative to.
 */
static void add_to_master_tree(ryml::Tree& master, const ryml::Tree& src,
                               size_t node, const std::string& src_path);

/**
 * Parse the file `ref` names as seen from `src_path`, store it in `master`
 * keyed by its resolved path, and walk it for further references.
 *
 * A path already in `master` is left alone. That both keeps one copy of a file
 * reached by several routes and stops a reference cycle: the key is stamped on
 * before the file is walked, so a file that reaches itself finds itself there.
 */
static void add_referenced_file(ryml::Tree& master, const std::string& src_path,
                                const std::string& ref) {
    const std::string path = resolve_path(src_path, ref);
    if (master.find_child(master.root_id(), ryml::to_csubstr(path)) !=
        ryml::NONE)
        return;

    ParsedData* child = static_cast<ParsedData*>(parse_file(path.c_str()));
    if (!child) return;  // reported where the reference is resolved
    ensure_capacity(master, 2);
    size_t dest = master.append_child(master.root_id());
    deep_copy_recursive(master, dest, child->tree, child->tree.root_id());
    // Root has no key, so stamp the path on after the copy
    master.ref(dest) |= ryml::KEY;
    master.set_key(dest, master.to_arena(ryml::to_csubstr(path)));
    add_to_master_tree(master, child->tree, child->tree.root_id(), path);
    delete child;
}

static void add_to_master_tree(ryml::Tree& master, const ryml::Tree& src,
                               size_t node, const std::string& src_path) {
    if (node == ryml::NONE || src.is_val(node)) return;
    for (size_t c = src.first_child(node); c != ryml::NONE;
         c = src.next_sibling(c)) {
        if (src.has_key(c) && src.key(c) == "include" && src.has_val(c)) {
            add_referenced_file(master, src_path,
                                val_str(src, c));
        } else if (src.has_key(c) && src.key(c) == "load" && src.is_seq(c)) {
            // A list of file names, with SELF standing for the naming file
            // itself rather than for a file to read.
            for (size_t e = src.first_child(c); e != ryml::NONE;
                 e = src.next_sibling(e)) {
                if (!src.has_val(e)) continue;
                std::string ref = val_str(src, e);
                if (ref != LOAD_SELF) add_referenced_file(master, src_path, ref);
            }
        } else if (src.has_key(c) && src.key(c) == "load" && src.has_val(c)) {
            std::string ref = val_str(src, c);
            if (ref != LOAD_SELF) add_referenced_file(master, src_path, ref);
        } else {
            add_to_master_tree(master, src, c, src_path);
        }
    }
}

/**
 * Makes the original lattice. Creates a tree that maps every file the document
 * is built from -- the top-level file and everything it reaches by `include` or
 * `load`, at any depth -- to that file's contents, keyed by its resolved path.
 *
 * `src` is the parsed top-level document, null if it could not be read or
 * parsed; it is freed here. `filename` is the path it is keyed by, which is also
 * what its `include` and `load` references resolve against.
 *
 * If the top-level file cannot be read or is not valid YAML, `parse_error` is
 * set to a human-readable description (with the offending line/column for a
 * syntax error) and the returned tree is an empty MAP. Callers treat a non-empty
 * `parse_error` as a fatal failure: there is no document to expand. A referenced
 * file that cannot be read is not fatal: it is simply missing from the master
 * tree, and reported when the reference to it is resolved.
 */
YAMLTreeHandle make_original(ParsedData* src, const std::string& filename,
                             std::string& parse_error) {
    ParsedData* master = new ParsedData();
    master->tree.rootref() |= ryml::MAP;
    if (src) {
        ensure_capacity(master->tree, 2);
        size_t dest = master->tree.append_child(master->tree.root_id());
        deep_copy_recursive(master->tree, dest, src->tree, src->tree.root_id());
        // Root has no key, so stamp the filename on after the copy
        master->tree.ref(dest) |= ryml::KEY;
        master->tree.set_key(dest,
                             master->tree.to_arena(ryml::to_csubstr(filename)));
        add_to_master_tree(master->tree, src->tree, src->tree.root_id(),
                           filename);
        delete src;
    } else {
        // parse_file recorded why on this thread; nothing else has parsed since.
        parse_error = yaml_last_parse_error();
        if (parse_error.empty()) parse_error = "parse failed";
    }
    return master;
}

}  // namespace pals
