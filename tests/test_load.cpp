#include "test_helpers.h"

// `load` commands: fundamentals.md (s:load). A load merges whole files subnode
// by subnode under the `PALS` root, where an include splices a file's contents
// in verbatim where it is written. Both are resolved into the `combined` tree.

namespace {

// One of the multi-file cases committed under tests/lattices, parsed through
// the file named. These are the tests that have to be about files on disk: a
// `load` or an `include` path is resolved relative to the file that writes it,
// so the case is a directory of files written relative to each other, and each
// one says in its own name what it is a case of.
struct LoadCase {
    std::string dir;
    struct lattices lat = {};

    explicit LoadCase(const std::string& name) : dir(name) {}

    ~LoadCase() {
        free_lattice_problems(lat.problems);
        delete_tree(lat.original);
        delete_tree(lat.combined);
        delete_tree(lat.expanded);
        delete_tree(lat.full_expanded);
        delete_tree(lat.adjunct);
    }

    void parse(const std::string& rel, const char* root_lattice = nullptr) {
        lat = parse_and_expand_PALS(lattice_file(dir + "/" + rel).c_str(),
                                    root_lattice);
    }

    std::vector<std::string> problems() const {
        std::vector<std::string> out;
        for (size_t i = 0; i < lat.problems.count; ++i)
            out.emplace_back(lat.problems.items[i].message);
        return out;
    }

    std::string joined_problems() const {
        std::string s;
        for (const std::string& p : problems()) s += p + "; ";
        return s;
    }

    // Only the problems the load itself raised. Most files here are a `PALS`
    // node and a few subnodes with no lattice in them at all, and the
    // "no lattice found to expand" that draws says nothing about the merge.
    std::string load_problems() const {
        std::string s;
        for (const std::string& p : problems())
            if (p.rfind("load:", 0) == 0) s += p + "; ";
        return s;
    }
};

bool any_contains(const std::vector<std::string>& msgs, const char* needle) {
    for (const std::string& m : msgs)
        if (m.find(needle) != std::string::npos) return true;
    return false;
}

YAMLNodeId pals_of(YAMLTreeHandle t) {
    return get_child_by_key(t, get_root(t), "PALS");
}

// The scalar values of a PALS list subnode (`notes`, `reminders`, ...).
std::vector<std::string> list_of(YAMLTreeHandle t, const char* key) {
    YAMLNodeId n = get_child_by_key(t, pals_of(t), key);
    std::vector<std::string> out;
    for (size_t i = 0; i < get_size(t, n); i++) {
        char* s = as_string(t, get_child_by_index(t, n, i));
        if (s) out.emplace_back(s);
        yaml_free_string(s);
    }
    return out;
}

// The names defined in `facility`, in order. Each entry is a single-key map
// wrapper and the key is the name.
std::vector<std::string> facility_names(YAMLTreeHandle t) {
    YAMLNodeId fac = facility_of(t);
    std::vector<std::string> out;
    for (size_t i = 0; i < get_size(t, fac); i++) {
        YAMLNodeId e = get_child_by_index(t, fac, i);
        char* k = get_node_key(t, get_child_by_index(t, e, 0));
        if (k) out.emplace_back(k);
        yaml_free_string(k);
    }
    return out;
}

std::string scalar_of(YAMLTreeHandle t, const char* key) {
    char* s = as_string(t, get_child_by_key(t, pals_of(t), key));
    std::string out = s ? s : "";
    yaml_free_string(s);
    return out;
}

// The keys of a PALS subnode, in order.
std::vector<std::string> keys_under(YAMLTreeHandle t, const char* key) {
    YAMLNodeId n = get_child_by_key(t, pals_of(t), key);
    std::vector<std::string> out;
    for (size_t i = 0; i < get_size(t, n); i++) {
        char* k = get_node_key(t, get_child_by_index(t, n, i));
        if (k) out.emplace_back(k);
        yaml_free_string(k);
    }
    return out;
}

}  // namespace

TEST_CASE("load merges whole files subnode by subnode", "[load]") {
    LoadCase c("load_basic");
    c.parse("joiner.pals.yaml");
    REQUIRE(c.joined_problems() == "");

    // List subnodes concatenate, keeping the order of the load list and, within
    // each file, the order written there.
    REQUIRE(list_of(c.lat.combined, "notes") ==
            std::vector<std::string>{"layout note", "settings note",
                                     "joiner note"});
    REQUIRE(facility_names(c.lat.combined) ==
            std::vector<std::string>{"Q1a", "main", "lat1", "use", "set"});

    // The directive itself does not survive into the combined tree.
    REQUIRE(get_child_by_key(c.lat.combined, pals_of(c.lat.combined), "load") ==
            YAML_NULL_ID);
}

TEST_CASE("a loaded settings file drives the lattice the layout file defines",
          "[load][lattices]") {
    // The point of the split: the merged file expands as though it had been
    // written as one, so the `set` from the settings file reaches the element
    // the layout file defined.
    LoadCase c("load_expand");
    c.parse("joiner.pals.yaml");
    REQUIRE(c.joined_problems() == "");

    YAMLNodeId q1a = find_by_key(c.lat.full_expanded, "Q1a");
    REQUIRE(q1a != YAML_NULL_ID);
    YAMLNodeId mult = get_child_by_key(c.lat.full_expanded, q1a, "MagneticMultipoleP");
    REQUIRE(mult != YAML_NULL_ID);
    REQUIRE(close(num_val(c.lat.full_expanded, get_child_by_key(c.lat.full_expanded, mult,
                                                           "Kn1")),
                  0.34));
}

TEST_CASE("SELF places the joiner's own contents in the load order", "[load]") {
    LoadCase c("load_self");
    c.parse("joiner.pals.yaml");
    REQUIRE(c.load_problems() == "");
    REQUIRE(list_of(c.lat.combined, "notes") ==
            std::vector<std::string>{"a", "joiner", "b"});
}

TEST_CASE("without SELF the joiner's own contents are merged last", "[load]") {
    LoadCase c("load_noself");
    c.parse("joiner.pals.yaml");
    REQUIRE(c.load_problems() == "");
    REQUIRE(list_of(c.lat.combined, "notes") ==
            std::vector<std::string>{"a", "joiner"});
}

TEST_CASE("load resolves nested loads bottom up", "[load]") {
    // `inner` is a complete file -- a.pals.yaml already merged into it -- before
    // it is merged into the joiner, and its own SELF refers to itself, not to
    // the joiner that loads it.
    LoadCase c("load_nested");
    c.parse("joiner.pals.yaml");
    REQUIRE(c.load_problems() == "");
    REQUIRE(list_of(c.lat.combined, "notes") ==
            std::vector<std::string>{"inner", "a", "joiner"});
}

TEST_CASE("a loaded file's includes are resolved before it is merged",
          "[load][include]") {
    // The included file is named relative to the file holding the include,
    // which in this case sits a directory below the joiner.
    LoadCase c("load_include");
    c.parse("joiner.pals.yaml");
    REQUIRE(c.load_problems() == "");
    REQUIRE(facility_names(c.lat.combined) ==
            std::vector<std::string>{"m1", "extra_ele", "m2"});
}

TEST_CASE("a load path is relative to the file naming it", "[load]") {
    // joiner reaches down into sub/, and sub/inner.pals.yaml reaches back up:
    // both are written relative to themselves, so neither depends on where the
    // process happens to be running.
    LoadCase c("load_relative");
    c.parse("joiner.pals.yaml");
    REQUIRE(c.load_problems() == "");
    REQUIRE(list_of(c.lat.combined, "notes") ==
            std::vector<std::string>{"top", "inner"});
}

TEST_CASE("distinct versions collect into one comma delimited list", "[load]") {
    LoadCase c("load_version");
    c.parse("joiner.pals.yaml");
    REQUIRE(c.load_problems() == "");
    // "1.0" is contributed twice and appears once; the joiner itself has no
    // version to add.
    REQUIRE(scalar_of(c.lat.combined, "version") == "1.0, 2.0");
}

TEST_CASE("one version shared by every file stays a single version", "[load]") {
    LoadCase c("load_version_same");
    c.parse("joiner.pals.yaml");
    REQUIRE(c.load_problems() == "");
    REQUIRE(scalar_of(c.lat.combined, "version") == "1.0");
}

TEST_CASE("Dict subnodes take the union and discard agreeing duplicates",
          "[load]") {
    LoadCase c("load_dict");
    c.parse("joiner.pals.yaml");
    REQUIRE(c.load_problems() == "");
    REQUIRE(keys_under(c.lat.combined, "extension_labels") ==
            std::vector<std::string>{"from_a", "shared", "from_joiner"});
}

TEST_CASE("two files disagreeing over a Dict entry is reported", "[load]") {
    LoadCase c("load_dict_clash");
    c.parse("joiner.pals.yaml");
    REQUIRE(any_contains(c.problems(), "extension_labels.shared"));

    // The earlier file in the load order keeps the value, so the disagreement
    // is reported rather than resolved silently either way.
    YAMLNodeId labels =
        get_child_by_key(c.lat.combined, pals_of(c.lat.combined),
                         "extension_labels");
    REQUIRE(val_eq(c.lat.combined,
                   get_child_by_key(c.lat.combined, labels, "shared"), "one"));
}

TEST_CASE("a load naming a file that cannot be read is reported", "[load]") {
    LoadCase c("load_missing");
    c.parse("joiner.pals.yaml");
    REQUIRE(any_contains(c.problems(), "no_such_file.pals.yaml"));
    // The rest of the document still combines: one unreadable file is not
    // grounds for throwing away the ones that were read.
    REQUIRE(list_of(c.lat.combined, "notes") ==
            std::vector<std::string>{"joiner"});
}

TEST_CASE("a cycle of loads is reported rather than followed", "[load]") {
    LoadCase c("load_cycle");
    c.parse("a.pals.yaml");
    REQUIRE(any_contains(c.problems(), "already being loaded"));
    // b's load of a is what closes the loop and is dropped; everything else
    // still merges.
    REQUIRE(list_of(c.lat.combined, "notes") ==
            std::vector<std::string>{"b", "a"});
}

TEST_CASE("a file loaded by two routes is read once", "[load]") {
    // `shared` is named by both branches. It is parsed once into `original`,
    // but each branch merges its own copy, so it contributes twice -- the merge
    // is over load lists, not over files.
    LoadCase c("load_diamond");
    c.parse("joiner.pals.yaml");
    REQUIRE(c.load_problems() == "");
    REQUIRE(list_of(c.lat.combined, "notes") ==
            std::vector<std::string>{"shared", "shared"});

    // One entry in `original` per file, whatever route reached it: the four
    // files here, no duplicate for the second route to `shared`.
    REQUIRE(get_size(c.lat.original, get_root(c.lat.original)) == 4);
}
