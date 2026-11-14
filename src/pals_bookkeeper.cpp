// The element bookkeeper: the walk along each branch that fills in what depends
// on where an element sits -- reference parameters, floor placement,
// s-position, element index -- and the dependent parameters derived from what
// the author gave; then girder placement and the Fork connection records. See
// pals_pipeline.h.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <ryml.hpp>
#include <ryml_std.hpp>

#include "apc/apc.h"
#include "pals_floor.h"
#include "pals_pipeline.h"
#include "pals_util.h"
#include "yaml_tree.h"

namespace pals {

// ============================================================
// ELEMENT BOOKKEEPING
// ============================================================
//
// Once every expression has been reduced to a number, each branch is walked
// element-by-element from its BeginningEle to compute the output parameters that
// depend on where an element sits in the branch: the reference parameters
// (species / energy / momentum / time), the floor placement, the s-position,
// and the field-dependent parameters (normalized <-> unnormalized multipole and
// bend strengths). See the "Lattice Expansion" step in
// pals/source/lattice-construction.md; the floor math (Eqs. wws etc.) lives in
// pals_floor.cpp.
//
// All ReferenceP / FloorP values describe the *upstream* end of their element.
// The bookkeeper persists each element's results into the tree before moving on,
// so bookkeep_element reads the previous element's stored parameters and
// "propagates" them onto the current one, exactly as the standard describes.

// Reference parameters at one end of an element (ReferenceP). Energy and
// momentum are related through the reference mass; a flag records which of them
// is known so complete_energy() can fill the other.
struct RefState {
    std::string species;
    double E_tot = 0.0;  // [eV] total energy
    double pc = 0.0;     // [eV] momentum * c
    double time = 0.0;   // [s]
    bool has_species = false;
    bool has_E = false;
    bool has_pc = false;
};

// Fill in whichever of E_tot / pc is missing from the other, using the reference
// species mass (E^2 = (pc)^2 + (m c^2)^2, all in eV). A no-op if the species is
// unknown, both are set, or neither is.
static void complete_energy(RefState& r) {
    if (!r.has_species) return;
    double m;
    try {
        m = apc::mass_of(r.species);
    } catch (...) {
        return;  // unknown species name; leave energy/momentum as-is
    }
    if (r.has_E && !r.has_pc) {
        double v = r.E_tot * r.E_tot - m * m;
        if (v >= 0.0) {
            r.pc = std::sqrt(v);
            r.has_pc = true;
        }
    } else if (r.has_pc && !r.has_E) {
        r.E_tot = std::sqrt(r.pc * r.pc + m * m);
        r.has_E = true;
    }
}

// Read the ReferenceP group of an element into a RefState (its upstream values).
static RefState read_ref(const ryml::Tree& t, size_t ele) {
    RefState r;
    size_t rp = t.find_child(ele, ryml::to_csubstr("ReferenceP"));
    if (rp == ryml::NONE) return r;
    r.has_species = get_str_child(t, rp, "species_ref", r.species);
    r.has_E = get_num_child(t, rp, "E_tot_ref", r.E_tot);
    r.has_pc = get_num_child(t, rp, "pc_ref", r.pc);
    get_num_child(t, rp, "time_ref", r.time);  // defaults to 0
    return r;
}

// Write a RefState back into an element's ReferenceP group (creating it).
static void write_ref(ryml::Tree& t, size_t ele, const RefState& r) {
    size_t rp = find_or_add_map_child(t, ele, "ReferenceP");
    if (r.has_species) set_str_child(t, rp, "species_ref", r.species);
    if (r.has_E) set_num_child(t, rp, "E_tot_ref", r.E_tot);
    if (r.has_pc) set_num_child(t, rp, "pc_ref", r.pc);
    set_num_child(t, rp, "time_ref", r.time);
}

// Apply a ReferenceChange element's ReferenceChangeP adjustments to a RefState
// (see referencechange.md): a species, an absolute or delta energy/momentum, and
// an absolute or delta reference time. Clearing has_E/has_pc lets the subsequent
// complete_energy() recompute the partner quantity with the new mass/energy.
// Only one of the energy settings, and one of the time settings, may be given;
// more is reported, and the first in the order below is used.
static void apply_ref_change(const ryml::Tree& t, size_t ele, RefState& r,
                             ProblemList& problems) {
    size_t rc = t.find_child(ele, ryml::to_csubstr("ReferenceChangeP"));
    if (rc == ryml::NONE || !t.is_map(rc)) return;

    std::string ename =
        key_str(t, ele);
    auto exclusive = [&](std::initializer_list<const char*> keys) {
        std::string given;
        int n = 0;
        for (const char* k : keys) {
            double unused;
            if (!get_num_child(t, rc, k, unused)) continue;
            given += (n++ ? ", " : "") + std::string(k);
        }
        if (n > 1)
            add_problem(problems,
                        "element '" + ename + "': ReferenceChangeP sets " +
                            given + "; only one of them may be given",
                        ename);
    };
    exclusive({"E_tot_ref", "pc_ref", "dE_ref", "dpc_ref"});
    exclusive({"time_ref", "dtime_ref"});

    std::string sp;
    if (get_str_child(t, rc, "species_ref", sp)) {
        r.species = sp;
        r.has_species = true;
        r.has_pc = false;  // recompute momentum for the new mass
    }
    double v;
    if (get_num_child(t, rc, "E_tot_ref", v)) {
        r.E_tot = v;
        r.has_E = true;
        r.has_pc = false;
    } else if (get_num_child(t, rc, "pc_ref", v)) {
        r.pc = v;
        r.has_pc = true;
        r.has_E = false;
    } else if (get_num_child(t, rc, "dE_ref", v)) {
        if (r.has_E) {
            r.E_tot += v;
            r.has_pc = false;
        }
    } else if (get_num_child(t, rc, "dpc_ref", v)) {
        if (r.has_pc) {
            r.pc += v;
            r.has_E = false;
        }
    }
    if (get_num_child(t, rc, "time_ref", v))
        r.time = v;
    else if (get_num_child(t, rc, "dtime_ref", v))
        r.time += v;
}

// Reference parameters at the downstream end of an element, given its completed
// upstream parameters. Species and energy carry through unchanged for most
// kinds; an RFCavity shifts the energy by RFP.dE_ref and a ReferenceChange
// applies its ReferenceChangeP. The reference time advances by the transit time
// (reference.md).
static RefState downstream_ref(const ryml::Tree& t, size_t ele,
                               const std::string& kind, const RefState& up,
                               double length, ProblemList& problems) {
    RefState down = up;

    if (kind == "RFCavity") {
        size_t rfp = t.find_child(ele, ryml::to_csubstr("RFP"));
        double dE;
        if (get_num_child(t, rfp, "dE_ref", dE) && down.has_E) {
            down.E_tot = up.E_tot + dE;
            down.has_pc = false;  // recompute momentum for the new energy
        }
    } else if (kind == "ReferenceChange") {
        apply_ref_change(t, ele, down, problems);
    } else if (kind == "Foil") {
        // A Foil can strip electrons, changing the downstream species (and hence
        // energy). The stripping model is not defined by the standard, so the
        // downstream species is left equal to the upstream one and the omission
        // is flagged rather than guessed at.
        std::string ename = t.has_key(ele) ? key_str(t, ele)
                                           : "<foil>";
        add_problem(problems,
                    "Foil element '" + ename +
                        "': downstream species change is not computed",
                    ename, Severity::Warning, Origin::Unsupported);
    }

    complete_energy(down);  // refill whichever of E/pc a change above cleared

    // Reference transit time: length * (E_up + E_down) / (c * (pc_up + pc_down)).
    // Added to down.time, not up.time: a ReferenceChange may have set or
    // shifted it above.
    if (up.has_E && down.has_E && up.has_pc && down.has_pc) {
        double denom = apc::C_LIGHT * (up.pc + down.pc);
        if (denom != 0.0)
            down.time += length * (up.E_tot + down.E_tot) / denom;
    }
    return down;
}

// Read an element's FloorP group into a FloorState. Missing components default
// to zero (origin / identity orientation). Returns false if the group is absent.
static bool read_floor(const ryml::Tree& t, size_t ele, FloorState& s) {
    size_t fp = t.find_child(ele, ryml::to_csubstr("FloorP"));
    if (fp == ryml::NONE) return false;
    double x = 0, y = 0, z = 0, th = 0, ph = 0, ps = 0;
    get_num_child(t, fp, "x", x);
    get_num_child(t, fp, "y", y);
    get_num_child(t, fp, "z", z);
    get_num_child(t, fp, "theta", th);
    get_num_child(t, fp, "phi", ph);
    get_num_child(t, fp, "psi", ps);
    s.r = Vec3{x, y, z};
    s.q = quat_from_floor_angles(FloorAngles{th, ph, ps});
    return true;
}

// Write a FloorState into a placement group of an element (FloorP or
// ShiftedFloorP, creating it), converting the orientation quaternion to
// (theta, phi, psi).
static void write_floor_group(ryml::Tree& t, size_t ele, const char* group,
                              const FloorState& s) {
    size_t fp = find_or_add_map_child(t, ele, group);
    set_num_child(t, fp, "x", s.r.x);
    set_num_child(t, fp, "y", s.r.y);
    set_num_child(t, fp, "z", s.r.z);
    FloorAngles a = floor_angles_from_quat(s.q);
    set_num_child(t, fp, "theta", a.theta);
    set_num_child(t, fp, "phi", a.phi);
    set_num_child(t, fp, "psi", a.psi);
}

static void write_floor(ryml::Tree& t, size_t ele, const FloorState& s) {
    write_floor_group(t, ele, "FloorP", s);
}

// The rigid shift (L, S) of an offset/rotation sextet -- a BodyShiftP, a
// PatchP, or the shift part of a CoordinateSetP -- as a placement relative to
// the frame it shifts (Eq. swww2).
static FloorState shift_of(const ryml::Tree& t, size_t group) {
    double xo = 0, yo = 0, zo = 0, xr = 0, yr = 0, zr = 0;
    get_num_child(t, group, "x_offset", xo);
    get_num_child(t, group, "y_offset", yo);
    get_num_child(t, group, "z_offset", zo);
    get_num_child(t, group, "x_rot", xr);
    get_num_child(t, group, "y_rot", yr);
    get_num_child(t, group, "z_rot", zr);
    FloorState sh;
    patch_LS(xo, yo, zo, xr, yr, zr, sh.r, sh.q);
    return sh;
}

// The bend angle and tilt of a Bend's reference curve: `angle_ref`, or `g_ref`
// times `length` when only the curvature is given.
static void bend_angle_tilt(const ryml::Tree& t, size_t ele, double length,
                            double& angle, double& tilt) {
    size_t bp = t.find_child(ele, ryml::to_csubstr("BendP"));
    angle = 0.0;
    tilt = 0.0;
    if (!get_num_child(t, bp, "angle_ref", angle)) {
        double g = 0.0;
        if (get_num_child(t, bp, "g_ref", g)) angle = g * length;
    }
    get_num_child(t, bp, "tilt_ref", tilt);
}

// Build the floor displacement L and coordinate rotation S for an element's
// reference curve, dispatched on kind: bends use their BendP angle/tilt, patches
// their PatchP offsets/rotations, everything else is a straight segment of the
// element length. (A FloorShift places its exit end outright; see
// coordinate_set_frame.)
static void element_LS(const ryml::Tree& t, size_t ele, const std::string& kind,
                       double length, Vec3& L, Quat& S) {
    if (kind == "Bend") {
        double angle, tilt;
        bend_angle_tilt(t, ele, length, angle, tilt);
        bend_LS(length, angle, tilt, L, S);
    } else if (kind == "Patch") {
        FloorState sh =
            shift_of(t, t.find_child(ele, ryml::to_csubstr("PatchP")));
        L = sh.r;
        S = sh.q;
    } else {
        straight_LS(length, L, S);
    }
}

// Two numbers agree "to high accuracy" (rf.md): equal within a relative
// tolerance, with both-zero counted as equal.
static bool approx_eq(double a, double b) {
    double scale = std::max(std::fabs(a), std::fabs(b));
    return scale == 0.0 || std::fabs(a - b) <= 1e-9 * scale;
}

// ---------------------------------------------------------------------------
// DEPENDENT PARAMETERS
//
// Many parameters within a group are not independent: one is a fixed multiple
// (or the reciprocal) of another. The expanded lattice holds every non-zero
// parameter, so when the author sets one member of such a relation the parser
// derives the rest. When the author sets more than one, the parser instead
// verifies they agree to high accuracy and flags an inconsistency (rather than
// silently overwriting). link_pair / reciprocal_link are the two primitives;
// the per-group resolvers below wire them up per the parameter docs.
// ---------------------------------------------------------------------------

// Relate two proportional parameters of a group, `b_key = factor * a_key`.
//  - both present  -> verify agreement, flag an inconsistency if not.
//  - only `a`      -> derive b = factor*a (unless the result is zero: a zero
//                     parameter is not "held").
//  - only `b`      -> derive a = b/factor (needs an invertible, non-zero factor).
// Returns true if it wrote a value, so callers can iterate to a fixed point.
static bool link_pair(ryml::Tree& t, size_t group, const char* a_key,
                      const char* b_key, double factor, const std::string& ctx,
                      ProblemList& problems) {
    double a, b;
    bool has_a = get_num_child(t, group, a_key, a);
    bool has_b = get_num_child(t, group, b_key, b);
    if (has_a && has_b) {
        double expect = factor * a;
        if (!approx_eq(b, expect)) {
            std::ostringstream m;
            m << ctx << ": '" << b_key << "' and '" << a_key
              << "' are inconsistent (" << b_key << " = " << format_double(b)
              << ", but " << format_double(factor) << " * " << a_key << " = "
              << format_double(expect) << ").";
            add_problem(problems, m.str());
        }
        return false;
    }
    if (has_a && factor * a != 0.0) {
        set_num_child(t, group, b_key, factor * a);
        return true;
    }
    if (has_b && factor != 0.0 && b / factor != 0.0) {
        set_num_child(t, group, a_key, b / factor);
        return true;
    }
    return false;
}

// Relate two reciprocal parameters of a group, `b_key = 1 / a_key` (e.g.
// `radius_ref = 1 / g_ref`). A zero value has no finite reciprocal, so it is
// neither a source nor a target. Consistency (a*b == 1) is only meaningful when
// both are non-zero. Returns true if it wrote a value.
static bool reciprocal_link(ryml::Tree& t, size_t group, const char* a_key,
                            const char* b_key, const std::string& ctx,
                            ProblemList& problems) {
    double a, b;
    bool has_a = get_num_child(t, group, a_key, a);
    bool has_b = get_num_child(t, group, b_key, b);
    if (has_a && has_b) {
        if (a != 0.0 && b != 0.0 && !approx_eq(a * b, 1.0)) {
            std::ostringstream m;
            m << ctx << ": '" << a_key << "' and '" << b_key
              << "' are inconsistent (" << a_key << " * " << b_key << " = "
              << format_double(a * b) << ", expected 1).";
            add_problem(problems, m.str());
        }
        return false;
    }
    if (has_a && a != 0.0) {
        set_num_child(t, group, b_key, 1.0 / a);
        return true;
    }
    if (has_b && b != 0.0) {
        set_num_child(t, group, a_key, 1.0 / b);
        return true;
    }
    return false;
}

// Store a value the bend geometry determines, or check the author's against it.
// Same contract as link_pair / reciprocal_link: an absent parameter is filled in
// (a zero is not "held"), a present one is verified and an inconsistency
// reported rather than silently overwritten.
static void derive_or_check(ryml::Tree& t, size_t node, const char* key,
                            double value, const std::string& ctx,
                            ProblemList& problems) {
    double have;
    if (get_num_child(t, node, key, have)) {
        if (!approx_eq(have, value)) {
            std::ostringstream m;
            m << ctx << ": '" << key
              << "' is inconsistent with the rest of the bend geometry (" << key
              << " = " << format_double(have) << ", but the geometry gives "
              << format_double(value) << ").";
            add_problem(problems, m.str());
        }
        return;
    }
    if (value != 0.0) set_num_child(t, node, key, value);
}

// sin(x)/x, continued through the removable singularity at the origin. The bend
// lengths are all of this shape, and taking them this way keeps a bend that is
// nearly (or exactly) straight on the same formulas as a curved one.
static double sinc(double x) { return x == 0.0 ? 1.0 : std::sin(x) / x; }

// Tie together the three interchangeable forms of a bend's curvature:
//   radius_ref = 1 / g_ref
//   g_ref      = factor * Bn0_ref     (factor = q*c/pc)
// Any one of them fills the other two; two that disagree are flagged. The field
// leg needs the reference momentum, so it runs only when `has_factor`.
static void link_bend_curvature(ryml::Tree& t, size_t bp, bool has_factor,
                                double factor, const std::string& ctx,
                                ProblemList& problems) {
    for (int pass = 0; pass < 3; ++pass) {
        bool changed =
            reciprocal_link(t, bp, "g_ref", "radius_ref", ctx, problems);
        if (has_factor)
            changed |=
                link_pair(t, bp, "Bn0_ref", "g_ref", factor, ctx, problems);
        if (!changed) break;
    }
}

// Resolve the BendP dependent parameters (bend.md, "Dependent parameters
// calculation").
//
// The shape of a bend is fixed by any two of: its curvature (`g_ref`,
// `Bn0_ref` or `radius_ref`), one of its lengths (`length`, `L_chord` or
// `L_rectangle`), and its bend angle (`angle_ref`) -- two taken from different
// ones of those three sets, which is exactly what the standard lets an author
// give. Reduced to the arc `length` and the `angle_ref` it turns through, the
// rest are (with angle = g_ref * length):
//   L_chord     = length * sinc(angle/2)     entrance origin to exit origin
//   L_rectangle = length * sinc(angle)       that chord along the entrance axis
//   L_sagitta   = length * angle/8 * sinc(angle/4)^2
// Written this way nothing divides by the curvature, so the straight bend is not
// a special case: at zero angle the three lengths collapse to `length`, `length`
// and zero on their own.
//
// Two lengths on their own (an arc and a chord, say) are left alone: they do fix
// the shape, but only through a transcendental inversion, and the standard does
// not allow the pair anyway. A value the author set is never overwritten -- it
// is checked against what the geometry gives, and an inconsistency reported.
static void resolve_bend(ryml::Tree& t, size_t ele, double& length,
                         bool has_factor, double factor,
                         const std::string& ename, ProblemList& problems) {
    size_t bp = t.find_child(ele, ryml::to_csubstr("BendP"));
    if (bp == ryml::NONE) return;
    std::string ctx = "element '" + ename + "' BendP";
    std::string ele_ctx = "element '" + ename + "'";

    link_bend_curvature(t, bp, has_factor, factor, ctx, problems);

    // What the author gave. A zero is no constraint: a bend of zero curvature,
    // zero angle or zero length says nothing about the rest of the geometry.
    double g = 0.0, angle = 0.0, L = 0.0, chord = 0.0, rect = 0.0;
    bool has_g = get_num_child(t, bp, "g_ref", g) && g != 0.0;
    bool has_angle = get_num_child(t, bp, "angle_ref", angle) && angle != 0.0;
    bool has_L = get_num_child(t, ele, "length", L) && L != 0.0;
    bool has_chord = get_num_child(t, bp, "L_chord", chord) && chord != 0.0;
    bool has_rect = get_num_child(t, bp, "L_rectangle", rect) && rect != 0.0;

    // Reduce whichever pair was given to (length, angle_ref). The curvature and
    // the arc give the angle directly; a chord or a rectangle needs an arcsine,
    // taken on the principal branch -- the bend does not turn past a half circle
    // (a quarter, for the rectangle).
    if (!has_angle && has_g) {
        if (has_L) {
            angle = g * L;
            has_angle = true;
        } else if (has_chord || has_rect) {
            // chord = 2 sin(angle/2) / g_ref;  rectangle = sin(angle) / g_ref
            double part = has_chord ? 2.0 : 1.0;
            double len = has_chord ? chord : rect;
            const char* key = has_chord ? "L_chord" : "L_rectangle";
            double sine = g * len / part;
            if (std::fabs(sine) > 1.0) {
                add_problem(problems,
                            ctx + ": '" + key + "' (" + format_double(len) +
                                ") is too long for the curvature 'g_ref' (" +
                                format_double(g) +
                                "); no bend of that radius reaches it.");
            } else {
                angle = part * std::asin(sine);
                has_angle = angle != 0.0;
            }
        }
    }
    // The arc that goes with the angle. A bend that has a length but neither
    // curvature nor angle keeps it, and stays straight.
    if (!has_L && has_angle) {
        if (has_g)
            L = angle / g;
        else if (has_chord)
            L = chord / sinc(angle / 2.0);
        else if (has_rect)
            L = rect / sinc(angle);
        has_L = L != 0.0;
    }

    if (has_L) {
        derive_or_check(t, ele, "length", L, ele_ctx, problems);
        derive_or_check(t, bp, "angle_ref", angle, ctx, problems);
        derive_or_check(t, bp, "g_ref", angle / L, ctx, problems);
        // radius_ref and Bn0_ref follow from a g_ref the geometry just supplied.
        link_bend_curvature(t, bp, has_factor, factor, ctx, problems);
        derive_or_check(t, bp, "L_chord", L * sinc(angle / 2.0), ctx, problems);
        derive_or_check(t, bp, "L_rectangle", L * sinc(angle), ctx, problems);
        double quarter = sinc(angle / 4.0);
        derive_or_check(t, bp, "L_sagitta",
                        L * angle / 8.0 * quarter * quarter, ctx, problems);
    }

    // The element length may have just been derived; later resolvers integrate
    // multipole components over it.
    get_num_child(t, ele, "length", length);
}

// Default the actual bending field from the reference one (bend.md,
// `Kn0_from_g_ref`). The field the particle actually sees is the dipole
// component of MagneticMultipoleP, and unless the author says otherwise a bend
// bends its own reference particle along the reference curve: `Kn0` = `g_ref`,
// equivalently `Bn0` = `Bn0_ref`. `Kn0_from_g_ref: false` leaves the component
// alone, for a bend whose actual field is zero or comes from somewhere else.
//
// All four forms of the dipole component count as "set" -- `Kn0L` and `Bn0L`
// state the same field as `Kn0` and `Bn0`, only integrated over the length --
// and only the normal ones: a skew dipole is a separate component, not this one.
// The group is created when the default applies to a bend that carries none,
// since otherwise the field would have nowhere to go. Whichever form is written
// here, resolve_magnetic_multipoles fills the other three next.
static void resolve_bend_actual_field(ryml::Tree& t, size_t ele) {
    size_t bp = t.find_child(ele, ryml::to_csubstr("BendP"));
    if (bp == ryml::NONE) return;
    std::string flag = child_val_str(t, bp, "Kn0_from_g_ref");
    if (!flag.empty() && !is_true_flag(flag)) return;  // absent means true

    size_t mp = t.find_child(ele, ryml::to_csubstr("MagneticMultipoleP"));
    if (mp != ryml::NONE)
        for (const char* k : {"Kn0", "Kn0L", "Bn0", "Bn0L"})
            if (t.find_child(mp, ryml::to_csubstr(k)) != ryml::NONE) return;

    // g_ref and Bn0_ref are tied together by the curvature linking above, so
    // g_ref is the one to take whenever the reference momentum was known;
    // Bn0_ref carries the field when it was not. A zero reference bend field is
    // no field at all, and a zero is not held.
    double g = 0.0, B = 0.0;
    const char* key = nullptr;
    double value = 0.0;
    if (get_num_child(t, bp, "g_ref", g) && g != 0.0) {
        key = "Kn0";
        value = g;
    } else if (get_num_child(t, bp, "Bn0_ref", B) && B != 0.0) {
        key = "Bn0";
        value = B;
    }
    if (!key) return;

    if (mp == ryml::NONE)
        mp = find_or_add_map_child(t, ele, "MagneticMultipoleP");
    set_num_child(t, mp, key, value);
}

// Split a multipole component key into its (normal/skew char, order digits),
// e.g. "Bn2L" -> ('n', "2"). `prefixes` is the pair of accepted leading tokens
// (magnetic "Bn"/"Bs"/"Kn"/"Ks" collapse to the field forms here; electric
// "En"/"Es"). Returns false for taper keys (an underscore) and anything that is
// not <prefix><digits>[L].
static bool parse_multipole_key(const std::string& k, char first,
                                char& ns_out, std::string& order_out) {
    if (k.size() < 3 || k[0] != first) return false;
    if (k[1] != 'n' && k[1] != 's') return false;
    if (k.find('_') != std::string::npos) return false;  // *_taper etc.
    std::string rest = k.substr(2);
    if (!rest.empty() && rest.back() == 'L') rest.pop_back();
    if (rest.empty()) return false;
    for (char c : rest)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    ns_out = k[1];
    order_out = rest;
    return true;
}

// Collect the set of (normal/skew, order) components present in a multipole
// group, judged by the field-form prefix `first` ('B' magnetic, 'E' electric)
// and its normalized partner `norm` ('K', or '\0' when there is none).
static std::set<std::pair<char, std::string>> multipole_components(
    const ryml::Tree& t, size_t group, char first, char norm) {
    std::set<std::pair<char, std::string>> comps;
    for (size_t c = t.first_child(group); c != ryml::NONE;
         c = t.next_sibling(c)) {
        if (!t.has_key(c)) continue;
        std::string k = key_str(t, c);
        char ns;
        std::string order;
        if (parse_multipole_key(k, first, ns, order) ||
            (norm && parse_multipole_key(k, norm, ns, order)))
            comps.emplace(ns, order);
    }
    return comps;
}

// Resolve MagneticMultipoleP components (magneticmultipole.md). Each component
// of order N has four equivalent forms tied by length and reference momentum:
//   BnNL = length * BnN,   KnN = factor * BnN,   KnNL = factor * length * BnN
// (and likewise for the skew "s" forms). Length-integration needs no momentum;
// the field<->normalized legs run only when `has_factor`. Iterated to a fixed
// point so a value given in any one form fills the rest.
static void resolve_magnetic_multipoles(ryml::Tree& t, size_t ele, double length,
                                        bool has_factor, double factor,
                                        const std::string& ename,
                                        ProblemList& problems) {
    size_t mp = t.find_child(ele, ryml::to_csubstr("MagneticMultipoleP"));
    if (mp == ryml::NONE) return;
    std::string ctx = "element '" + ename + "' MagneticMultipoleP";

    for (const auto& comp : multipole_components(t, mp, 'B', 'K')) {
        std::string B = std::string("B") + comp.first + comp.second;
        std::string BL = B + "L";
        std::string K = std::string("K") + comp.first + comp.second;
        std::string KL = K + "L";
        for (int pass = 0; pass < 4; ++pass) {
            bool changed = false;
            changed |= link_pair(t, mp, B.c_str(), BL.c_str(), length, ctx,
                                 problems);
            changed |= link_pair(t, mp, K.c_str(), KL.c_str(), length, ctx,
                                 problems);
            if (has_factor) {
                changed |= link_pair(t, mp, B.c_str(), K.c_str(), factor, ctx,
                                     problems);
                changed |= link_pair(t, mp, BL.c_str(), KL.c_str(), factor, ctx,
                                     problems);
            }
            if (!changed) break;
        }
    }
}

// Resolve ElectricMultipoleP components (electricmultipole.md). Electric
// multipoles have no normalized form, so a component of order N is only tied by
// length-integration: EnNL = length * EnN (and the skew "s" forms). No momentum
// is needed.
static void resolve_electric_multipoles(ryml::Tree& t, size_t ele, double length,
                                        const std::string& ename,
                                        ProblemList& problems) {
    size_t ep = t.find_child(ele, ryml::to_csubstr("ElectricMultipoleP"));
    if (ep == ryml::NONE) return;
    std::string ctx = "element '" + ename + "' ElectricMultipoleP";

    for (const auto& comp : multipole_components(t, ep, 'E', '\0')) {
        std::string E = std::string("E") + comp.first + comp.second;
        std::string EL = E + "L";
        link_pair(t, ep, E.c_str(), EL.c_str(), length, ctx, problems);
    }
}

// Compute the field-dependent output parameters of an element. Uses the
// element's *upstream* reference (parameters are referenced to the upstream
// end). The reference momentum sets the field<->normalized conversion factor
// K = factor * B; when the species/momentum is unknown the momentum-dependent
// legs are skipped, but the purely geometric relations (bend angle/radius,
// multipole length-integration) still run.
//
// `length` is in/out: a bend given its angle and curvature but no length has one
// derived, and the multipole integrations that follow use it.
static void compute_dependent(ryml::Tree& t, size_t ele, const std::string& kind,
                              const RefState& up, double& length,
                              ProblemList& problems) {
    bool has_factor = false;
    double factor = 0.0;
    if (up.has_species && up.has_pc && up.pc > 0.0) {
        try {
            factor = apc::charge_of(up.species) * apc::C_LIGHT / up.pc;
            has_factor = true;
        } catch (...) {
            has_factor = false;
        }
    }

    std::string ename = ele_name(t, ele);
    if (kind == "Bend") {
        resolve_bend(t, ele, length, has_factor, factor, ename, problems);
        // Before the multipoles: this seeds the dipole component they resolve.
        resolve_bend_actual_field(t, ele);
    }
    resolve_magnetic_multipoles(t, ele, length, has_factor, factor, ename,
                                problems);
    resolve_electric_multipoles(t, ele, length, ename, problems);
}

// Resolve the RFP dependent parameters (rf.md). The active length `L_active`
// defaults to the element `length`; it is materialized when non-zero. The RF
// voltage and gradient are then tied by `voltage = gradient * L_active`: one is
// derived from the other, or an inconsistency is flagged when both are set.
// (frequency <-> harmonic_number is *not* resolved here: it needs the change in reference
// time across the whole branch, which is not known element-by-element.) Any
// element carrying an RFP group is handled, keyed on the group's presence.
static void compute_rf_dependent(ryml::Tree& t, size_t ele, double length,
                                 ProblemList& problems) {
    size_t rfp = t.find_child(ele, ryml::to_csubstr("RFP"));
    if (rfp == ryml::NONE) return;

    // L_active defaults to the element length; hold it when non-zero.
    double L_active = 0.0;
    if (!get_num_child(t, rfp, "L_active", L_active)) {
        L_active = length;
        if (L_active != 0.0) set_num_child(t, rfp, "L_active", L_active);
    }

    std::string ctx = "element '" + ele_name(t, ele) + "' RFP";
    link_pair(t, rfp, "gradient", "voltage", L_active, ctx, problems);
}

// Parameters with a non-zero (or enum/boolean) default, filled in for any group
// that is *present* in the element. The expanded lattice holds every non-zero
// parameter, so an author who writes a group but omits these gets the defaults
// made explicit. A group absent from the element is left untouched -- it is
// added only by the parser (ReferenceP, FloorP) or to hold a derived value with
// nowhere else to go (MagneticMultipoleP on a curved bend, see
// resolve_bend_actual_field). Parameters whose default is zero/null/false are
// not listed: they are not "held".
struct GroupDefault {
    const char* group;
    const char* key;
    const char* value;
};
static const GroupDefault kGroupDefaults[] = {
    {"RFP", "cavity_type", "STANDING_WAVE"},
    {"RFP", "zero_phase", "ACCELERATING"},
    {"BendP", "ref_geometry", "ARC"},
    {"BendP", "multipole_geometry", "FOLLOWS_REF_GEOMETRY"},
    {"BendP", "Kn0_from_g_ref", "true"},
    {"ApertureP", "shape", "ELLIPTICAL"},
    {"ApertureP", "location", "ENTRANCE_END"},
    {"ApertureP", "aperture_active", "true"},
    {"CoordinateSetP", "origin_ele", "GLOBAL_ORIGIN"},
    {"CoordinateSetP", "origin_ele_ref_pt", "EXIT_END"},
    {"ForkP", "propagate_reference", "true"},
};

// Write the non-zero/enum defaults (kGroupDefaults) into every group the element
// already carries, leaving any value the author set in place.
static void materialize_group_defaults(ryml::Tree& t, size_t ele) {
    for (const GroupDefault& d : kGroupDefaults) {
        size_t g = t.find_child(ele, ryml::to_csubstr(d.group));
        if (g == ryml::NONE || !t.is_map(g)) continue;
        if (t.find_child(g, ryml::to_csubstr(d.key)) == ryml::NONE)
            set_plain_child(t, g, d.key, d.value);
    }
}

// Where a branch's first element takes its reference and floor from when the
// branch was made by a Fork (fork.md, s:forking): the Fork's floor always, its
// reference only when `propagate_reference` is true.
struct ForkSeed {
    size_t fork = ryml::NONE;
    bool reference = false;
};

// The per-element bookkeeper. Given the previous element in the branch (NONE for
// the first, the BeginningEle) and the current element, compute and store the
// current element's upstream reference parameters, floor placement, s-position,
// and field-dependent parameters. Order matters: the reference momentum is
// needed for the dependent-parameter conversions, so reference comes first.
//
// `seed` names the Fork element that instantiated this branch, for the first
// element of a branch a Fork created. `exits` holds the exit placements of the
// elements that set theirs outright (FloorShift), which take the place of
// propagating through them.
static void bookkeep_element(ryml::Tree& t, size_t prev, size_t ele,
                                ProblemList& problems,
                                const std::map<size_t, FloorState>& exits,
                                ForkSeed seed = ForkSeed()) {
    std::string kind = child_val_str(t, ele, "kind");

    RefState up;
    FloorState floor;
    double s_pos = 0.0;

    if (prev == ryml::NONE) {
        // First element of the branch (BeginningEle): reference, floor and
        // s-position are user inputs. Complete the reference (fill pc from E or
        // vice versa) and normalize the floor placement; both default sensibly
        // when unset (zero energy/momentum, origin, identity orientation, s = 0).
        // A branch a Fork made takes the Fork's floor instead, and its reference
        // too when the Fork propagates it. A Fork has zero length and a unit
        // transfer map, so its stored (upstream) reference and floor are also
        // its values at the connection point. The s-coordinate of the new
        // branch still starts at zero.
        bool take_ref = seed.fork != ryml::NONE && seed.reference;
        up = read_ref(t, take_ref ? seed.fork : ele);
        complete_energy(up);
        read_floor(t, seed.fork != ryml::NONE ? seed.fork : ele, floor);
        if (seed.fork == ryml::NONE) get_num_child(t, ele, "s_position", s_pos);
    } else {
        // Any later element: its upstream parameters are the previous element's
        // downstream parameters, propagated through the previous element.
        std::string pkind = child_val_str(t, prev, "kind");
        double plen = 0.0;
        get_num_child(t, prev, "length", plen);

        RefState pup = read_ref(t, prev);
        complete_energy(pup);
        up = downstream_ref(t, prev, pkind, pup, plen, problems);

        auto ex = exits.find(prev);
        if (ex != exits.end()) {
            floor = ex->second;
        } else {
            FloorState pfloor;
            read_floor(t, prev, pfloor);
            Vec3 L;
            Quat S;
            element_LS(t, prev, pkind, plen, L, S);
            floor = floor_propagate(pfloor, L, S);
        }

        double ps = 0.0;
        get_num_child(t, prev, "s_position", ps);
        s_pos = ps + plen;
    }

    // Store the outputs. Reference before dependent: the latter reads pc_ref.
    write_ref(t, ele, up);
    write_floor(t, ele, floor);
    set_num_child(t, ele, "s_position", s_pos);

    // Fill in dependent parameters and non-zero/enum defaults of the groups the
    // element carries. The expanded lattice holds every non-zero parameter.
    double length = 0.0;
    get_num_child(t, ele, "length", length);
    materialize_group_defaults(t, ele);
    compute_dependent(t, ele, kind, up, length, problems);
    compute_rf_dependent(t, ele, length, problems);
}

// ---------------------------------------------------------------------------
// FLOOR PLACEMENT: FloorShift, Fiducial, Girder and ShiftedFloorP
// ---------------------------------------------------------------------------
//
// Most elements are placed by propagating along the branch. Three kinds place
// something outright, through a CoordinateSetP (coordinateset.md): a coordinate
// system taken at a reference point of an origin element (or the global origin),
// shifted by the group's offsets and rotations, and then by the element's own
// BodyShiftP.
//
//  - A FloorShift sets the placement of its exit end, and so of everything
//    downstream of it, leaving everything upstream alone.
//  - A Fiducial (zero length) fixes its own placement, and so moves the elements
//    both upstream and downstream of it: the stretch of the branch around it,
//    from the branch start or the last FloorShift to the next FloorShift, is
//    one rigid piece, and is carried bodily to where the Fiducial says.
//  - A Girder (outside the lattice) has a nominal placement from its
//    CoordinateSetP, and moves the elements it supports by its BodyShiftP.
//
// A branch is therefore placed in segments, each ending at a FloorShift or at
// the branch end. Once a segment is propagated, a Fiducial in it is applied,
// and only then is the segment final: a FloorShift exit, a later Fiducial, or a
// Girder may refer to an element only once that element's placement is final.

// Everything the floor pass needs to know across branches.
struct FloorContext {
    size_t lat_node = ryml::NONE;
    std::map<size_t, FloorState> exits;  // FloorShift exit placements
    std::set<size_t> final_floor;              // elements whose placement is final
    // Fixed placements that are not a Fiducial's own: the destination of a Fork
    // into a new branch, when that is not the branch's first element.
    std::map<size_t, FloorState> fixed;
};

// The placement of an element's exit end.
static FloorState exit_floor(const ryml::Tree& t, size_t ele,
                                   const FloorContext& ctx) {
    auto ex = ctx.exits.find(ele);
    if (ex != ctx.exits.end()) return ex->second;
    FloorState f;
    read_floor(t, ele, f);
    double len = 0.0;
    get_num_child(t, ele, "length", len);
    Vec3 L;
    Quat S;
    element_LS(t, ele, child_val_str(t, ele, "kind"), len, L, S);
    return floor_propagate(f, L, S);
}

// The frame, relative to an element's entrance placement, about which its
// BodyShiftP is measured: the middle of a straight element, the middle of the
// chord of a bend (coordinates.md, s:straight.mis and s:bend.mis). A Patch has
// no body to shift and is measured from its entrance.
static FloorState body_reference(const ryml::Tree& t, size_t ele) {
    std::string kind = child_val_str(t, ele, "kind");
    double len = 0.0;
    get_num_child(t, ele, "length", len);
    FloorState ref;
    if (kind == "Patch") return ref;
    if (kind == "Bend") {
        double angle, tilt;
        bend_angle_tilt(t, ele, len, angle, tilt);
        bend_LS(len / 2, angle / 2, tilt, ref.r, ref.q);
        if (std::fabs(angle) > 1e-12) {
            // Mid-arc to mid-chord, toward the centre of curvature.
            double rho = len / angle;
            double d = rho * (std::cos(angle / 2) - 1.0);
            FloorState chord;
            chord.r = Vec3{d * std::cos(tilt), d * std::sin(tilt), 0.0};
            ref = floor_compose(ref, chord);
        }
        return ref;
    }
    straight_LS(len / 2, ref.r, ref.q);
    return ref;
}

// The placement of the coordinate system an element's CoordinateSetP describes,
// shifted further by the element's BodyShiftP when `with_body` is set. `ele` is
// the element holding the group; `prev_exit` the exit of the element before it
// in its branch (null when there is none, or none whose placement is final
// yet). Returns false, with a problem recorded, when the origin cannot be
// placed.
static bool coordinate_set_frame(const ryml::Tree& t, size_t ele,
                                 const FloorState* prev_exit,
                                 const FloorContext& ctx, bool with_body,
                                 FloorState& out, ProblemList& problems) {
    std::string name = ele_name(t, ele);
    size_t cs = t.find_child(ele, ryml::to_csubstr("CoordinateSetP"));
    std::string origin = "GLOBAL_ORIGIN";
    std::string ref_pt = "EXIT_END";
    if (cs != ryml::NONE) {
        get_str_child(t, cs, "origin_ele", origin);
        get_str_child(t, cs, "origin_ele_ref_pt", ref_pt);
    }

    FloorState base;  // the global origin
    if (origin == "PREVIOUS_ELEMENT") {
        if (!prev_exit) {
            add_problem(problems,
                        "element '" + name +
                            "': CoordinateSetP.origin_ele is PREVIOUS_ELEMENT, "
                            "but there is no previous element whose placement "
                            "is known first",
                        name);
            return false;
        }
        base = *prev_exit;
    } else if (origin != "GLOBAL_ORIGIN") {
        ElementMatches m = match_element_parameters(t, ctx.lat_node, origin);
        if (!m.valid || m.has_param || m.elements.size() != 1) {
            add_problem(problems,
                        "element '" + name + "': CoordinateSetP.origin_ele '" +
                            origin + "' must name exactly one lattice element (" +
                            std::to_string(m.elements.size()) + " matched)",
                        name);
            return false;
        }
        size_t o = m.elements[0];
        if (!ctx.final_floor.count(o)) {
            add_problem(problems,
                        "element '" + name + "': the placement of "
                        "CoordinateSetP.origin_ele '" + origin +
                            "' is not known before that of '" + name + "'",
                        name);
            return false;
        }
        if (ref_pt == "ENTRANCE_END") {
            read_floor(t, o, base);
        } else if (ref_pt == "CENTER") {
            FloorState in;
            read_floor(t, o, in);
            std::string okind = child_val_str(t, o, "kind");
            double len = 0.0;
            get_num_child(t, o, "length", len);
            FloorState half;
            if (okind == "Bend") {
                double angle, tilt;
                bend_angle_tilt(t, o, len, angle, tilt);
                bend_LS(len / 2, angle / 2, tilt, half.r, half.q);
            } else {
                straight_LS(len / 2, half.r, half.q);
            }
            base = floor_compose(in, half);
        } else {
            base = exit_floor(t, o, ctx);
        }
    }

    out = cs != ryml::NONE ? floor_compose(base, shift_of(t, cs)) : base;
    size_t bs = t.find_child(ele, ryml::to_csubstr("BodyShiftP"));
    if (with_body && bs != ryml::NONE)
        out = floor_compose(out, shift_of(t, bs));
    return true;
}

// Finish one segment of a branch: carry it bodily to where a Fiducial in it (or
// a fixed fork destination) puts it, then mark every element of it final.
// `prev_exit` is the exit of the element just before the segment, when that is
// final (the end of the previous segment), for a Fiducial placed relative to it.
static void finish_floor_segment(ryml::Tree& t,
                                 const std::vector<size_t>& seg,
                                 const FloorState* prev_exit,
                                 FloorContext& ctx, ProblemList& problems) {
    size_t anchor = ryml::NONE;
    FloorState fixed;
    for (size_t i = 0; i < seg.size(); ++i) {
        size_t e = seg[i];
        bool fiducial = child_val_str(t, e, "kind") == "Fiducial";
        auto fx = ctx.fixed.find(e);
        if (!fiducial && fx == ctx.fixed.end()) continue;
        if (anchor != ryml::NONE) {
            add_problem(problems,
                        "elements '" + ele_name(t, anchor) + "' and '" +
                            ele_name(t, e) +
                            "' both fix the placement of the same rigid "
                            "stretch of their branch; only the first is used",
                        ele_name(t, e));
            continue;
        }
        FloorState where;
        if (fiducial) {
            // PREVIOUS_ELEMENT is usable only when the element before the
            // Fiducial is outside this segment, and so already final.
            const FloorState* pe = i == 0 ? prev_exit : nullptr;
            if (!coordinate_set_frame(t, e, pe, ctx, true, where, problems))
                continue;
        } else {
            where = fx->second;
        }
        anchor = e;
        fixed = where;
    }

    if (anchor != ryml::NONE) {
        FloorState now;
        read_floor(t, anchor, now);
        FloorState motion =
            floor_compose(fixed, floor_inverse(now));
        for (size_t e : seg) {
            FloorState f;
            read_floor(t, e, f);
            write_floor(t, e, floor_compose(motion, f));
        }
    }
    for (size_t e : seg) ctx.final_floor.insert(e);
}

// Write a placement into an element's ShiftedFloorP group (creating it).
static void write_shifted_floor(ryml::Tree& t, size_t ele,
                                const FloorState& s) {
    write_floor_group(t, ele, "ShiftedFloorP", s);
}

// The `girders` dict of the PALS node: girder name -> definition.
static std::vector<size_t> girder_defs(const ryml::Tree& t) {
    std::vector<size_t> out;
    size_t pals = t.find_child(t.root_id(), ryml::to_csubstr("PALS"));
    if (pals == ryml::NONE) return out;
    size_t g = t.find_child(pals, ryml::to_csubstr("girders"));
    if (g == ryml::NONE || !t.is_map(g)) return out;
    for (size_t c = t.first_child(g); c != ryml::NONE; c = t.next_sibling(c))
        if (t.is_map(c) && t.has_key(c) &&
            child_val_str(t, c, "kind") == "Girder")
            out.push_back(c);
    return out;
}

// Place the girders and fill in every element's ShiftedFloorP
// (shiftedfloor.md): its placement with its BodyShiftP and the shifts of the
// girders that support it (coordinates.md, s:girder.mis).
//
// A girder's nominal placement comes from its CoordinateSetP and is its FloorP;
// its BodyShiftP then moves it, and with it everything it supports, as one
// rigid body. Written as a rigid motion of the floor, the girder G moves things
// by M_G = F_G0 * Shift_G * F_G0^-1, after the motion of the girder supporting
// it, if any. An element E with reference frame F_E0 and its own shift Shift_E
// ends up at M_G * F_E0 * Shift_E, which is Eqs. lwwl-vewe.
static void place_girders_and_shifts(ryml::Tree& t, FloorContext& ctx,
                                     ProblemList& problems) {
    std::vector<size_t> girders = girder_defs(t);

    // What each girder supports, and so which girder supports what.
    std::map<size_t, size_t> support;  // element or girder -> its girder
    for (size_t g : girders) {
        std::string gname = ele_name(t, g);
        size_t gp = t.find_child(g, ryml::to_csubstr("GirderP"));
        std::string spec;
        if (!get_str_child(t, gp, "elements", spec)) {
            add_problem(problems,
                        "Girder '" + gname + "': GirderP.elements names nothing "
                        "to support",
                        gname);
            continue;
        }
        std::vector<size_t> others;
        for (size_t o : girders)
            if (o != g) others.push_back(o);
        ElementMatches m =
            match_element_parameters(t, ctx.lat_node, spec, others);
        if (!m.valid || m.has_param) {
            add_problem(problems,
                        "Girder '" + gname + "': GirderP.elements '" + spec +
                            "' is not an element set",
                        gname);
            continue;
        }
        if (m.elements.empty())
            add_problem(problems,
                        "Girder '" + gname + "': GirderP.elements '" + spec +
                            "' matches nothing in the expanded lattice",
                        gname, Severity::Warning);
        for (size_t e : m.elements) {
            auto it = support.find(e);
            if (it != support.end() && it->second != g) {
                add_problem(problems,
                            "'" + ele_name(t, e) + "' is supported by both "
                            "Girder '" + ele_name(t, it->second) +
                                "' and Girder '" + gname + "'",
                            ele_name(t, e));
                continue;
            }
            support[e] = g;
        }
    }

    // Each girder's nominal placement and motion, parents first.
    std::map<size_t, FloorState> nominal, motion;
    std::set<size_t> failed;
    std::function<bool(size_t, std::set<size_t>&)> place =
        [&](size_t g, std::set<size_t>& active) -> bool {
        if (motion.count(g)) return true;
        if (failed.count(g)) return false;
        std::string gname = ele_name(t, g);
        size_t cs = t.find_child(g, ryml::to_csubstr("CoordinateSetP"));
        std::string origin;
        if (cs != ryml::NONE && get_str_child(t, cs, "origin_ele", origin) &&
            origin == "PREVIOUS_ELEMENT") {
            add_problem(problems,
                        "Girder '" + gname + "': CoordinateSetP.origin_ele may "
                        "not be PREVIOUS_ELEMENT -- a Girder sits in no branch",
                        gname);
            failed.insert(g);
            return false;
        }
        FloorState f0;
        if (!coordinate_set_frame(t, g, nullptr, ctx, false, f0, problems)) {
            failed.insert(g);
            return false;
        }
        FloorState parent;  // identity: no supporting girder
        auto sp = support.find(g);
        if (sp != support.end()) {
            if (active.count(sp->second)) {
                add_problem(problems,
                            "Girder '" + gname + "' is part of a circular chain "
                            "of girder support",
                            gname);
                failed.insert(g);
                return false;
            }
            active.insert(g);
            bool ok = place(sp->second, active);
            active.erase(g);
            if (!ok) {
                failed.insert(g);
                return false;
            }
            parent = motion[sp->second];
        }
        FloorState shift;
        size_t bs = t.find_child(g, ryml::to_csubstr("BodyShiftP"));
        if (bs != ryml::NONE) shift = shift_of(t, bs);
        nominal[g] = f0;
        motion[g] = floor_compose(
            parent, floor_compose(
                        f0, floor_compose(shift, floor_inverse(f0))));
        return true;
    };
    for (size_t g : girders) {
        std::set<size_t> active{g};
        if (!place(g, active)) continue;
        write_floor(t, g, nominal[g]);
        write_shifted_floor(t, g, floor_compose(motion[g], nominal[g]));
    }

    // ShiftedFloorP of every element of every branch.
    for_each_line_element(t, ctx.lat_node, [&](size_t, size_t, size_t ele,
                                               size_t) {
        if (!t.is_map(ele)) return;
        std::string kind = child_val_str(t, ele, "kind");
        if (kind == "Placeholder") return;
        FloorState in;
        if (!read_floor(t, ele, in)) return;
        // The BodyShiftP of a FloorShift or Fiducial places the element rather
        // than misaligning it, so its two placements are the same.
        if (kind == "FloorShift" || kind == "Fiducial") {
            write_shifted_floor(t, ele, in);
            return;
        }
        FloorState ref = body_reference(t, ele);
        FloorState body = floor_compose(in, ref);
        size_t bs = t.find_child(ele, ryml::to_csubstr("BodyShiftP"));
        if (bs != ryml::NONE) body = floor_compose(body, shift_of(t, bs));
        auto sp = support.find(ele);
        if (sp != support.end() && motion.count(sp->second))
            body = floor_compose(motion[sp->second], body);
        write_shifted_floor(t, ele, floor_compose(body, floor_inverse(ref)));
    });
}

// Stamp `element_index: n` on an element definition: the element's position in
// the branch line that holds it, counting from one.
//
// Every entry of the line is counted, so the index is the array index the
// element sits at even when an entry before it is a bare name the expansion
// could not resolve and so has no definition to fill in. The `branch_end` cap
// is an element of the line like any other and is numbered with the rest.
static void set_element_index(ryml::Tree& t, size_t ele, size_t index) {
    set_plain_child(t, ele, "element_index", std::to_string(index));
}

// Append a zero-length `Placeholder` element named `branch_end` to a branch's
// `line` and return its definition node. This is the marker that holds the
// branch's final floor placement and reference parameters; the bookkeeper fills
// those in by treating it like any other element (its upstream parameters are
// the downstream end of the last real element).
static size_t append_branch_end(ryml::Tree& t, size_t line) {
    ensure_capacity(t, 4);
    size_t wrapper = t.append_child(line);  // the anonymous seq-entry map
    t.ref(wrapper) |= ryml::MAP;
    size_t def = t.append_child(wrapper);  // the keyed element definition
    t.ref(def) |= ryml::KEY | ryml::MAP;
    t.set_key(def, t.to_arena(ryml::to_csubstr("branch_end")));
    append_keyval(t, def, "kind", "Placeholder");
    return def;
}

// Report a branch whose reference parameters cannot be computed.
//
// Every element takes its reference from the one before it, so the whole branch
// rests on the first element's: it is either written on that element (a
// BeginningEle's ReferenceP) or propagated in from the Fork that created the
// branch. When neither happens there is no species and no energy to propagate,
// and nothing later in the branch can supply them -- a `ReferenceP` further
// down is read as that element's own upstream values, not as a starting point
// for the ones before it. The species and the energy are reported separately
// because either alone is enough to leave the reference incomplete: without the
// species there is no mass to convert energy and momentum through, and without
// either energy or momentum there is nothing to convert.
//
// Runs after the first element has been bookkept, so `ele` holds the reference
// it ended up with, seed and all.
static void check_branch_reference(const ryml::Tree& t, size_t branch,
                                   size_t ele, ProblemList& problems) {
    RefState r = read_ref(t, ele);
    if (r.has_species && (r.has_E || r.has_pc)) return;

    const char* missing =
        !r.has_species
            ? (r.has_E || r.has_pc ? "no reference species"
                                   : "no reference species or energy")
            : "no reference energy or momentum";
    std::string branch_name = t.has_key(branch) ? key_str(t, branch) : "<branch>";
    add_problem(problems,
                "branch '" + branch_name + "': first element '" +
                    ele_name(t, ele) + "' has " + missing +
                    ", and none was propagated into the branch; the reference "
                    "parameters cannot be computed");
}

// True if `ele` is the first element of the line of one of the branches of the
// lattice `lat_node`.
static bool is_first_of_line(const ryml::Tree& t, size_t lat_node, size_t ele) {
    for (size_t branch : branch_nodes(t, lat_node)) {
        size_t line = branch_line(t, branch);
        if (line == ryml::NONE) continue;
        for (size_t le = t.first_child(line); le != ryml::NONE;
             le = t.next_sibling(le)) {
            size_t def = line_entry_element(t, le);
            if (def == ryml::NONE) continue;
            if (def == ele) return true;
            break;
        }
    }
    return false;
}

// The element a Fork leads to: the node id handle_fork left in its
// `ForkP.destination_pointer`, or ryml::NONE when it has none (the Fork failed
// to resolve, and was reported then). The id is the work tree's: the pointer is
// remapped only once the lattice is cut out into a tree of its own.
static size_t fork_destination(const ryml::Tree& t, size_t fork) {
    size_t forkp = t.find_child(fork, ryml::to_csubstr("ForkP"));
    std::string ptr = child_val_str(t, forkp, "destination_pointer");
    size_t dest;
    if (ptr.empty() || !parse_node_id(ptr, dest)) return ryml::NONE;
    return dest;
}

// True if a Fork instantiated a new branch. handle_fork resolves
// `ForkP.new_branch` to the name of the branch it made, or to `null` for a fork
// into a branch that already existed.
static bool fork_made_branch(const ryml::Tree& t, size_t fork) {
    std::string nb = child_val_str(
        t, t.find_child(fork, ryml::to_csubstr("ForkP")), "new_branch");
    return !nb.empty() && nb != "null";
}

void run_element_bookkeeper(ryml::Tree& t, size_t lat_node,
                            ProblemList& problems) {
    // Each branch's line is walked in order, threading each element's results
    // (persisted in the tree) into the next. After the branch's elements, a
    // `branch_end` Placeholder is appended and bookkept to hold the branch's
    // final floor placement and reference parameters (the downstream end of the
    // last element). The floor placement is settled segment by segment as the
    // walk goes (see FLOOR PLACEMENT), and once every branch is placed the
    // girders are, and every element's ShiftedFloorP.
    //
    // A Fork into a new branch seeds that branch's beginning element from its
    // own floor, and from its reference when its `propagate_reference` is true;
    // when the destination is further into the branch, its placement is fixed
    // to the Fork's instead. Branches are visited in order and a new branch is
    // always appended after the Fork that creates it, so by the time a forked
    // branch is reached its Fork has been bookkept and placed: `fork_seed` maps
    // the destination element (by node id, matching the raw
    // `destination_pointer`) to that Fork.
    if (lattice_branches(t, lat_node) == ryml::NONE) return;

    FloorContext ctx;
    ctx.lat_node = lat_node;
    std::map<size_t, ForkSeed> fork_seed;  // destination element -> its Fork
    std::vector<size_t> forks;             // in walk order

    for (size_t branch : branch_nodes(t, lat_node)) {
        size_t line = branch_line(t, branch);
        if (line == ryml::NONE) continue;

        // A Fork's placement is final once its segment is, so the fixed
        // placements of destinations deeper into new branches are recorded only
        // then; they are all in place before the branches they fix are walked.
        for (size_t f : forks) {
            size_t dest = fork_destination(t, f);
            FloorState ff;
            if (dest != ryml::NONE && fork_made_branch(t, f) &&
                !fork_seed.count(dest) && read_floor(t, f, ff))
                ctx.fixed[dest] = ff;
        }
        forks.clear();

        size_t prev = ryml::NONE;
        size_t index = 0;  // position in `line` of the entry being visited
        std::vector<size_t> seg;
        bool have_seg_entry = false;
        FloorState seg_entry;  // exit of the element before the segment
        for (size_t le = t.first_child(line); le != ryml::NONE;
             le = t.next_sibling(le)) {
            ++index;
            // Bare unresolved references have no parameters.
            size_t def = line_entry_element(t, le);
            if (def == ryml::NONE) continue;

            bool first = (prev == ryml::NONE);
            ForkSeed seed;
            if (first) {
                auto it = fork_seed.find(def);
                if (it != fork_seed.end()) seed = it->second;
            }
            bookkeep_element(t, prev, def, problems, ctx.exits, seed);
            set_element_index(t, def, index);
            if (first) check_branch_reference(t, branch, def, problems);
            seg.push_back(def);

            std::string kind = child_val_str(t, def, "kind");
            if (kind == "FloorShift") {
                // The segment ends here: settle it, then place the exit, which
                // may lean on anything already final -- this segment included.
                finish_floor_segment(t, seg, have_seg_entry ? &seg_entry : nullptr,
                                     ctx, problems);
                seg.clear();
                FloorState in, out;
                read_floor(t, def, in);
                if (!coordinate_set_frame(t, def, &in, ctx, true, out, problems))
                    out = in;
                ctx.exits[def] = out;
                seg_entry = out;
                have_seg_entry = true;
            }

            // Record where a Fork leads. Seeding applies only when the
            // destination is the beginning element of a *new* branch (fork.md):
            // a fork into an existing branch, which handle_fork resolves to
            // `new_branch: null`, never seeds.
            if (kind == "Fork") {
                size_t dest = fork_destination(t, def);
                if (dest != ryml::NONE && fork_made_branch(t, def)) {
                    std::string prop = child_val_str(
                        t, t.find_child(def, ryml::to_csubstr("ForkP")),
                        "propagate_reference");
                    if (is_first_of_line(t, lat_node, dest))
                        fork_seed[dest] = ForkSeed{def, prop != "false"};
                    forks.push_back(def);
                }
            }
            prev = def;
        }

        // An empty branch has no final state to record; otherwise cap it with a
        // `branch_end` Placeholder carrying the downstream end of the last
        // element. A second pass (after post-expansion sets) walks a branch that
        // already carries the one the first pass appended: the loop above has
        // just bookkept it like any other element, so there is nothing to add.
        bool capped = prev != ryml::NONE &&
                      t.key(prev) == ryml::to_csubstr("branch_end");
        if (prev != ryml::NONE && !capped) {
            size_t cap = append_branch_end(t, line);
            bookkeep_element(t, prev, cap, problems, ctx.exits);
            // Appended past the last entry the loop counted, so it takes the
            // next index.
            set_element_index(t, cap, index + 1);
            seg.push_back(cap);
        }
        finish_floor_segment(t, seg, have_seg_entry ? &seg_entry : nullptr, ctx,
                             problems);
    }

    place_girders_and_shifts(t, ctx, problems);
}

// Map every element definition in the expanded lattice to the
// `{branch-name}>>{element-name}` form the fork parameters name elements by
// (fork.md, forkfrom.md). Built as a whole before the forks are walked because a
// Fork names a destination in a branch other than its own, which the walk has
// either already passed or not yet reached.
static std::map<size_t, std::string> qualified_element_names(
    const ryml::Tree& t, size_t lat_node) {
    std::map<size_t, std::string> names;
    for (size_t branch : branch_nodes(t, lat_node)) {
        if (!t.has_key(branch)) continue;
        std::string branch_name = key_str(t, branch);
        size_t line = branch_line(t, branch);
        if (line == ryml::NONE) continue;
        for (size_t le = t.first_child(line); le != ryml::NONE;
             le = t.next_sibling(le)) {
            size_t ele = line_entry_element(t, le);
            if (ele != ryml::NONE)
                names[ele] = branch_name + ">>" + key_str(t, ele);
        }
    }
    return names;
}

/**
 * Write the two names that record, in the expanded lattice, what each `Fork`
 * connected to: `ForkP.forked_to` on the Fork, and `ForkFromP` on its
 * destination.
 *
 * `forked_to` (fork.md, s:fork.params) is an output parameter naming the
 * destination element as `{branch-name}>>{element-name}` -- the resolved form of
 * the `to_line`/`destination_element` the input asked for, so whatever the input
 * carried under that key is replaced by what the Fork actually reached.
 *
 * `ForkFromP` (forkfrom.md, s:fork.from.params) points the other way: a sequence
 * on the destination carrying one entry per incoming Fork, keyed
 * `{branch-name}>>{element-name}` after the Fork and its branch, with the
 * Fork's index in that branch's line as the value:
 *
 *     ForkFromP:
 *       - inject_line>>inj_fork: 134
 *       - alt_line>>end_fork: 37
 *
 * The index is 1-based -- entry 37 is the 37th element of `alt_line`. Both names
 * are built by the parser and exist only in the expanded lattice, so like
 * `destination_pointer` their nodes carry no provenance.
 *
 * Runs after expansion, when every branch line is final and the indices are the
 * ones the expanded lattice actually carries. The `destination_pointer`
 * scalars still hold work tree ids at this point -- remap_destination_pointers
 * renumbers them only once the lattice has been cut out into a tree of its own
 * -- so each one names its destination element directly.
 */
void link_fork_connections(ryml::Tree& t, size_t lat_node) {
    if (lattice_branches(t, lat_node) == ryml::NONE) return;
    std::map<size_t, std::string> qnames = qualified_element_names(t, lat_node);

    for (size_t branch : branch_nodes(t, lat_node)) {
        if (!t.has_key(branch)) continue;
        std::string branch_name = key_str(t, branch);
        size_t line = branch_line(t, branch);
        if (line == ryml::NONE) continue;

        int index = 0;
        for (size_t le = t.first_child(line); le != ryml::NONE;
             le = t.next_sibling(le)) {
            // Every entry counts towards the index, including a bare name that
            // stayed unresolved -- the index is a position in the line, not a
            // count of the elements that expanded.
            index++;
            size_t ele = line_entry_element(t, le);
            if (ele == ryml::NONE) continue;
            if (child_val_str(t, ele, "kind") != "Fork") continue;

            size_t dest = fork_destination(t, ele);
            if (dest == ryml::NONE || dest >= t.capacity() || !t.is_map(dest))
                continue;

            // Name the destination on the Fork. A destination that is not in
            // `qnames` is not an element of any branch line, which handle_fork
            // does not produce a pointer to.
            size_t forkp = t.find_child(ele, ryml::to_csubstr("ForkP"));
            auto dest_name = qnames.find(dest);
            if (dest_name != qnames.end())
                set_plain_child(t, forkp, "forked_to", dest_name->second);

            size_t group = find_or_add_seq_child(t, dest, "ForkFromP");
            ensure_capacity(t, 2);
            size_t item = t.append_child(group);
            t.ref(item) |= ryml::MAP;
            append_keyval(t, item, (branch_name + ">>" + key_str(t, ele)).c_str(),
                          std::to_string(index));
        }
    }
}

}  // namespace pals
