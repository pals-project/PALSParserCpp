// ============================================================
// FLOOR COORDINATE GEOMETRY
// ============================================================
//
// Direct unit tests for the quaternion floor geometry in pals_floor.cpp. The
// element bookkeeper exercises the in-plane bend path indirectly, but the
// angle<->quaternion round trip (including the gimbal-lock branch), tilted
// bends, and the patch builder have no coverage there. These tests pin the
// pure functions in isolation, against the closed-form geometry.
//
// pals_floor.cpp is compiled straight into the tests executable (see
// tests/CMakeLists.txt): its symbols carry hidden visibility and so are not
// reachable through the PALSParserCpp shared library.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

#include "../src/pals_floor.h"

using namespace pals;

namespace {

constexpr double kPi = 3.14159265358979323846;

void chk(double a, double b) { REQUIRE(a == Catch::Approx(b).margin(1e-9)); }

void chk_vec(const Vec3& v, double x, double y, double z) {
    chk(v.x, x);
    chk(v.y, y);
    chk(v.z, z);
}

// Two orientations are equal iff they rotate the three basis vectors the same
// way. Comparing the rotation action (rather than the quaternion components)
// sidesteps the q / -q double cover and is the right invariant for the
// angle round-trip, where only the encoded rotation is defined.
void chk_same_rotation(const Quat& a, const Quat& b) {
    for (const Vec3& e : {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}}) {
        Vec3 ra = quat_rotate(a, e);
        Vec3 rb = quat_rotate(b, e);
        chk(ra.x, rb.x);
        chk(ra.y, rb.y);
        chk(ra.z, rb.z);
    }
}

}  // namespace

TEST_CASE("quat_rotate applies elementary rotations", "[floor]") {
    // R_y(theta) sends +z to (sin theta, 0, cos theta); +x to (cos theta, 0,
    // -sin theta). This fixes the sign convention the rest of the module relies
    // on.
    const double a = 0.3;
    chk_vec(quat_rotate(quat_rot_y(a), Vec3{0, 0, 1}), std::sin(a), 0, std::cos(a));
    chk_vec(quat_rotate(quat_rot_y(a), Vec3{1, 0, 0}), std::cos(a), 0, -std::sin(a));

    // R_x(a): +z -> (0, -sin a, cos a). R_z(a): +x -> (cos a, sin a, 0).
    chk_vec(quat_rotate(quat_rot_x(a), Vec3{0, 0, 1}), 0, -std::sin(a), std::cos(a));
    chk_vec(quat_rotate(quat_rot_z(a), Vec3{1, 0, 0}), std::cos(a), std::sin(a), 0);
}

TEST_CASE("quat_mul composes rotations (a applied after b)", "[floor]") {
    // quat_mul(a, b) rotates by b first, then a. Rotating +x by R_z(pi/2) gives
    // +y, then R_y(pi/2) sends +y to +y (unchanged, y is the R_y axis)... so use
    // a case that actually distinguishes order: R_x(pi/2) then R_z(pi/2).
    Quat c = quat_mul(quat_rot_z(kPi / 2), quat_rot_x(kPi / 2));
    // b = R_x(pi/2): +z -> +... R_x(pi/2) sends +y->+z, +z->-y. Take +y:
    //   R_x(pi/2): +y -> +z; then R_z(pi/2): +z -> +z. Net +y -> +z.
    chk_vec(quat_rotate(c, Vec3{0, 1, 0}), 0, 0, 1);
    // Reversing the factors must give a different result, proving order matters.
    Quat c_rev = quat_mul(quat_rot_x(kPi / 2), quat_rot_z(kPi / 2));
    Vec3 v = quat_rotate(c_rev, Vec3{0, 1, 0});
    REQUIRE_FALSE(v.z == Catch::Approx(1.0).margin(1e-9));
}

TEST_CASE("FloorP angles round-trip through the quaternion", "[floor]") {
    // General orientation away from the gimbal singularity: recovering the angles
    // and rebuilding must reproduce the same rotation.
    FloorAngles a{0.3, 0.2, -0.4};
    Quat q = quat_from_floor_angles(a);
    FloorAngles b = floor_angles_from_quat(q);
    chk(b.theta, a.theta);
    chk(b.phi, a.phi);
    chk(b.psi, a.psi);
    chk_same_rotation(quat_from_floor_angles(b), q);
}

TEST_CASE("A positive phi tilts the z-axis toward +Y (Bmad convention)", "[floor]") {
    // W(1,2) = sin(phi): the z-axis column of W is (sin(theta)cos(phi), sin(phi),
    // cos(theta)cos(phi)).
    FloorAngles a{0.3, 0.2, -0.4};
    Vec3 z = quat_rotate(quat_from_floor_angles(a), Vec3{0, 0, 1});
    chk(z.x, std::sin(0.3) * std::cos(0.2));
    chk(z.y, std::sin(0.2));
    chk(z.z, std::cos(0.3) * std::cos(0.2));
}

TEST_CASE("floor_angles_from_quat handles the +pi/2 gimbal lock", "[floor]") {
    // At phi = +pi/2, theta and psi share an axis; as in Bmad, theta is pinned
    // to 0 and psi carries the rotation. The recovered angles must still encode
    // the original orientation exactly.
    FloorAngles a{0.5, kPi / 2, 0.3};
    Quat q = quat_from_floor_angles(a);
    FloorAngles b = floor_angles_from_quat(q);

    chk(b.phi, kPi / 2);
    chk(b.theta, 0.0);
    chk_same_rotation(quat_from_floor_angles(b), q);
}

TEST_CASE("floor_angles_from_quat handles the -pi/2 gimbal lock", "[floor]") {
    FloorAngles a{-0.7, -kPi / 2, 0.9};
    Quat q = quat_from_floor_angles(a);
    FloorAngles b = floor_angles_from_quat(q);

    chk(b.phi, -kPi / 2);
    chk(b.theta, 0.0);
    chk_same_rotation(quat_from_floor_angles(b), q);
}

TEST_CASE("straight_LS is a pure z-advance with no rotation", "[floor]") {
    Vec3 L;
    Quat S;
    straight_LS(2.5, L, S);
    chk_vec(L, 0, 0, 2.5);
    chk_same_rotation(S, Quat{1, 0, 0, 0});  // identity
}

TEST_CASE("bend_LS in the horizontal plane matches the arc geometry", "[floor]") {
    // rho = length / angle = 10. Untilted: displacement stays in the x-z plane
    // and the downstream z-axis turns to (-sin angle, 0, cos angle).
    const double len = 1.5, ang = 0.15;
    const double rho = len / ang;
    Vec3 L;
    Quat S;
    bend_LS(len, ang, 0.0, L, S);

    chk_vec(L, rho * (std::cos(ang) - 1.0), 0.0, rho * std::sin(ang));
    chk_vec(quat_rotate(S, Vec3{0, 0, 1}), -std::sin(ang), 0.0, std::cos(ang));
    // A horizontal bend introduces azimuth only: phi = psi = 0, theta = -angle.
    FloorAngles a = floor_angles_from_quat(S);
    chk(a.theta, -ang);
    chk(a.phi, 0.0);
    chk(a.psi, 0.0);
}

TEST_CASE("bend_LS with tilt pi/2 bends vertically", "[floor]") {
    // tilt_ref = pi/2 rotates the bend plane into y-z: the displacement moves
    // into -y and the downstream z-axis tips downward to (0, -sin angle,
    // cos angle). tilt_ref = +pi/2 is a downward bend, and the heading has to
    // follow the displacement rather than tip against it.
    const double len = 1.5, ang = 0.15;
    const double rho = len / ang;
    Vec3 L;
    Quat S;
    bend_LS(len, ang, kPi / 2, L, S);

    chk_vec(L, 0.0, rho * (std::cos(ang) - 1.0), rho * std::sin(ang));
    chk_vec(quat_rotate(S, Vec3{0, 0, 1}), 0.0, -std::sin(ang), std::cos(ang));
}

TEST_CASE("bend_LS keeps the frame tangent to the arc at any tilt", "[floor]") {
    // The general statement behind the two cases above: whatever tilt_ref does to
    // the bend plane, the downstream z-axis must be the tangent to the arc that
    // L traces. Eqs. ustt and srrr are two spellings of that one rotation, so
    // they must agree; they do only for u's first component = +sin(tilt_ref).
    const double len = 1.5, ang = 0.15;
    for (double tilt : {0.0, 0.3, kPi / 2, 2.0, kPi, -0.7}) {
        Vec3 L;
        Quat S;
        bend_LS(len, ang, tilt, L, S);

        // Eq. srrr: S = R_z(tilt) R_y(-ang) R_z(-tilt).
        Quat srrr = quat_mul(quat_rot_z(tilt),
                             quat_mul(quat_rot_y(-ang), quat_rot_z(-tilt)));
        chk_same_rotation(S, srrr);

        // And the heading is the arc's tangent: R_z(tilt) applied to the untilted
        // tangent (-sin ang, 0, cos ang).
        Vec3 tangent = quat_rotate(quat_rot_z(tilt),
                                   Vec3{-std::sin(ang), 0.0, std::cos(ang)});
        Vec3 zaxis = quat_rotate(S, Vec3{0, 0, 1});
        chk_vec(zaxis, tangent.x, tangent.y, tangent.z);
    }
}

TEST_CASE("bend_LS with a ~zero angle degenerates to a straight segment",
          "[floor]") {
    Vec3 L;
    Quat S;
    bend_LS(2.0, 0.0, 0.0, L, S);
    chk_vec(L, 0, 0, 2.0);
    chk_same_rotation(S, Quat{1, 0, 0, 0});
}

TEST_CASE("patch_LS carries offsets directly and composes its rotations",
          "[floor]") {
    Vec3 L;
    Quat S;

    // Pure offset, no rotation: L is the offset vector; S is the identity.
    patch_LS(0.1, -0.2, 0.3, 0.0, 0.0, 0.0, L, S);
    chk_vec(L, 0.1, -0.2, 0.3);
    chk_same_rotation(S, Quat{1, 0, 0, 0});

    // Pure y_rot: S = R_y(y_rot), so +z tips to (sin y_rot, 0, cos y_rot).
    patch_LS(0, 0, 0, 0.0, 0.25, 0.0, L, S);
    chk_vec(quat_rotate(S, Vec3{0, 0, 1}), std::sin(0.25), 0.0, std::cos(0.25));

    // Full composition S = R_y * R_x * R_z must match a hand-built product.
    patch_LS(0, 0, 0, 0.1, 0.2, 0.3, L, S);
    Quat expect = quat_mul(quat_rot_y(0.2), quat_mul(quat_rot_x(0.1), quat_rot_z(0.3)));
    chk_same_rotation(S, expect);
}

TEST_CASE("floor_propagate accumulates position and orientation", "[floor]") {
    // Two straight segments along +z from the origin add their lengths.
    Vec3 L;
    Quat S;
    FloorState s{{0, 0, 0}, {1, 0, 0, 0}};
    straight_LS(2.0, L, S);
    s = floor_propagate(s, L, S);
    straight_LS(3.0, L, S);
    s = floor_propagate(s, L, S);
    chk_vec(s.r, 0, 0, 5.0);

    // A straight step taken in an already-rotated frame advances along that
    // frame's z-axis: starting at R_y(theta), a length-l drift moves by
    // (l sin theta, 0, l cos theta).
    const double theta = 0.4, l = 2.0;
    FloorState r{{0, 0, 0}, quat_rot_y(theta)};
    straight_LS(l, L, S);
    r = floor_propagate(r, L, S);
    chk_vec(r.r, l * std::sin(theta), 0.0, l * std::cos(theta));
    chk_same_rotation(r.q, quat_rot_y(theta));  // straight step leaves orientation

    // A horizontal bend then rotates the frame; the composed orientation has
    // azimuth -angle.
    bend_LS(1.5, 0.15, 0.0, L, S);
    FloorState b = floor_propagate(FloorState{{0, 0, 0}, {1, 0, 0, 0}}, L, S);
    chk(floor_angles_from_quat(b.q).theta, -0.15);
}
