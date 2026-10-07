#ifndef PALS_FLOOR_H
#define PALS_FLOOR_H

/**
 * @file pals_floor.h
 * @brief Low-level floor (global) coordinate geometry for lattice expansion.
 *
 * Implements the "Floor Coordinates" and "Lattice Element Positioning" sections
 * of the PALS standard (pals/source/coordinates.md). A floor placement is the
 * position and orientation of the branch coordinate system at one point along
 * the reference curve; the standard writes the orientation as the 3x3 rotation
 * matrix W (Eq. www), but here it is carried as a unit quaternion instead. Only
 * the boundary conversions to/from the FloorP orientation angles
 * (theta, phi, psi) touch the matrix form at all.
 *
 * The element-by-element construction (the branch bookkeeper in
 * pals_expand.cpp) walks the reference curve using floor_propagate():
 * given the placement at the upstream end of an element and the element's local
 * displacement L and coordinate rotation S, it returns the placement at the
 * downstream end (Eq. wws). straight_LS / bend_LS / patch_LS build the (L, S)
 * pair for the three reference-curve geometries PALS defines.
 */

namespace pals {

// A vector in floor (global) Cartesian coordinates, or a displacement expressed
// in a local branch frame.
struct Vec3 {
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

// A unit quaternion (w + x i + y j + z k) representing a rotation. Used in place
// of the 3x3 orientation matrix W: the branch x, y, z axes are the columns of
// the matrix this quaternion encodes.
struct Quat {
    double w = 1.0;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
};

// A floor placement: the origin position r of the branch coordinate system and
// its orientation q (the quaternion form of the W matrix), at one point on the
// reference curve.
struct FloorState {
    Vec3 r;
    Quat q;
};

// The three FloorP orientation angles (radians), as defined in the Floor
// Coordinates section: theta (azimuth), phi (pitch), psi (roll).
struct FloorAngles {
    double theta = 0.0;
    double phi = 0.0;
    double psi = 0.0;
};

// --- Quaternion primitives ---

// Hamilton product a*b. With quat_rotate below this composes rotations so that
// quat_mul(a, b) applies b first, then a (matrix product A*B).
Quat quat_mul(const Quat& a, const Quat& b);

// Return a copy scaled to unit norm (identity if the input is ~zero). Repeated
// products accumulate rounding, so placements are renormalized as they are
// carried along the branch.
Quat quat_normalize(const Quat& q);

// Rotate vector v by the rotation q encodes, i.e. compute W*v where W is the
// matrix form of q.
Vec3 quat_rotate(const Quat& q, const Vec3& v);

// Quaternion for a rotation of `angle` radians about `axis` (need not be
// normalized; a ~zero axis yields the identity).
Quat quat_from_axis_angle(const Vec3& axis, double angle);

// Quaternions for the elementary rotations R_x, R_y, R_z of the standard
// (Eq. wtt0t), about the branch x, y, z axes respectively.
Quat quat_rot_x(double angle);
Quat quat_rot_y(double angle);
Quat quat_rot_z(double angle);

// --- FloorP angle <-> quaternion (Eq. www) ---

// Build the orientation quaternion from FloorP angles:
//   W = R_y(theta) * R_x(-phi) * R_z(psi).
// As in Bmad, a positive phi tilts the z-axis toward +Y.
Quat quat_from_floor_angles(const FloorAngles& a);

// Recover the FloorP angles from an orientation quaternion (inverse of
// quat_from_floor_angles), as in Bmad's floor_w_mat_to_angles. At the
// phi = +-pi/2 gimbal singularity theta is taken to be zero and psi absorbs the
// remaining rotation.
FloorAngles floor_angles_from_quat(const Quat& q);

// --- Reference-curve propagation ---

// Placement at the downstream end of an element given the placement s0 at its
// upstream end, the element's displacement vector L (in the upstream branch
// frame), and its coordinate rotation S:
//   r1 = r0 + W0*L,   W1 = W0*S     (Eq. wws).
FloorState floor_propagate(const FloorState& s0, const Vec3& L, const Quat& S);

// --- (L, S) builders for the three reference-curve geometries ---

// Straight element (Drift, Quadrupole, ...) of the given length: L = (0,0,L),
// S = identity (Eq. l00l).
void straight_LS(double length, Vec3& L, Quat& S);

// Bend with arc length `length`, reference bend angle `angle` (radians), and
// `tilt_ref` (radians). Uses Eqs. ustt / lrztt. A ~zero angle degenerates to a
// straight element of that length.
void bend_LS(double length, double angle, double tilt_ref, Vec3& L, Quat& S);

// Patch element (Eq. lxyz): L = (x_offset, y_offset, z_offset),
// S = R_y(y_rot) * R_x(x_rot) * R_z(z_rot).
void patch_LS(double x_offset, double y_offset, double z_offset, double x_rot,
              double y_rot, double z_rot, Vec3& L, Quat& S);

}  // namespace pals

#endif  // PALS_FLOOR_H
