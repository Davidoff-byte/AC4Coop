#include "HookMath.h"

#include <cmath>

// Our own implementation. Conventions: row-major storage, row vectors (v' = v * M), translation in row 4.
// The engine layouts in HookMath.h are facts about the binary; the maths below is ours.
namespace HookMath
{
	static inline float Sqrt(float v) { return v > 0.0f ? sqrtf(v) : 0.0f; }

	// ---- basic vector/quaternion length ---------------------------------------------------------
	float Modulo(Vector3 in) { return Sqrt(in.x * in.x + in.y * in.y + in.z * in.z); }
	float Modulo(Vector4 in) { return Sqrt(in.x * in.x + in.y * in.y + in.z * in.z + in.w * in.w); }
	float Modulo(Quaternion in) { return Sqrt(in.x * in.x + in.y * in.y + in.z * in.z + in.w * in.w); }

	Vector3 Normalize(Vector3 in)
	{
		float m = Modulo(in);
		if (m <= 1e-8f) return Vector3{};
		float inv = 1.0f / m;
		return Vector3{ in.x * inv, in.y * inv, in.z * inv };
	}

	Vector4 Normalize(Vector4 in)
	{
		float m = Modulo(in);
		if (m <= 1e-8f) return Vector4{};
		float inv = 1.0f / m;
		return Vector4{ in.x * inv, in.y * inv, in.z * inv, in.w * inv };
	}

	Quaternion Normalize(Quaternion in)
	{
		float m = Modulo(in);
		if (m <= 1e-8f) return Quaternion{};
		float inv = 1.0f / m;
		return Quaternion{ in.x * inv, in.y * inv, in.z * inv, in.w * inv };
	}

	// ---- dot / cross ----------------------------------------------------------------------------
	float Dot(Vector3 a, Vector3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
	float Dot(Vector4 a, Vector4 b) { return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w; }
	float __fastcall QuatDotProduct(Quaternion a, Quaternion b)
	{
		return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
	}

	Vector3 Cross(Vector3 a, Vector3 b)
	{
		return Vector3{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
	}

	Vector4 Cross(Vector4 a, Vector4 b)
	{
		return Vector4{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x, 0.0f };
	}

	// ---- interpolation --------------------------------------------------------------------------
	float __fastcall lerp(float a, float b, double t) { return (float)(a + (b - a) * t); }

	Vector3 __fastcall vlerp(Vector3 a, Vector3 b, double t)
	{
		return Vector3{ lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t) };
	}

	Vector4 __fastcall vlerp(Vector4 a, Vector4 b, double t)
	{
		return Vector4{ lerp(a.x, b.x, t), lerp(a.y, b.y, t), lerp(a.z, b.z, t), lerp(a.w, b.w, t) };
	}

	Quaternion __fastcall slerp(Quaternion a, Quaternion b, double t)
	{
		float d = QuatDotProduct(a, b);
		if (d < 0.0f) { b = Quaternion{ -b.x, -b.y, -b.z, -b.w }; d = -d; }
		if (d > 0.9995f) {
			float f = (float)t;
			return Normalize(Quaternion{
				a.x + (b.x - a.x) * f, a.y + (b.y - a.y) * f,
				a.z + (b.z - a.z) * f, a.w + (b.w - a.w) * f });
		}
		float theta = acosf(d);
		float sinT = sinf(theta);
		float wa = sinf((float)((1.0 - t) * theta)) / sinT;
		float wb = sinf((float)(t * theta)) / sinT;
		return Quaternion{ a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb };
	}

	// ---- Euler <-> quaternion (heading = Y, attitude = Z, bank = X; degrees) --------------------
	Quaternion __fastcall EulerAnglesToQuaternion(float Heading, float Attitude, float Bank)
	{
		const float d2r = 3.14159265358979323846f / 180.0f;
		float h = Heading * d2r * 0.5f, a = Attitude * d2r * 0.5f, b = Bank * d2r * 0.5f;
		float sh = sinf(h), ch = cosf(h), sa = sinf(a), ca = cosf(a), sb = sinf(b), cb = cosf(b);
		return Quaternion{
			ca * sb * ch - sa * cb * sh,
			sa * cb * ch + ca * sb * sh,
			ca * cb * sh - sa * sb * ch,
			ca * cb * ch + sa * sb * sh
		};
	}

	Vector3 __fastcall QuaternionToEuler(Quaternion q)
	{
		const float r2d = 180.0f / 3.14159265358979323846f;
		float sinp = 2.0f * (q.w * q.x + q.y * q.z);
		float cosp = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
		float bank = atan2f(sinp, cosp);
		float siny = 2.0f * (q.w * q.y - q.z * q.x);
		siny = siny > 1.0f ? 1.0f : (siny < -1.0f ? -1.0f : siny);
		float heading = asinf(siny);
		float sinr = 2.0f * (q.w * q.z + q.x * q.y);
		float cosr = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
		float attitude = atan2f(sinr, cosr);
		return Vector3{ heading * r2d, attitude * r2d, bank * r2d };
	}

	Quaternion MakeRotation(float angleDegrees, float axisX, float axisY, float axisZ)
	{
		float m = Sqrt(axisX * axisX + axisY * axisY + axisZ * axisZ);
		if (m <= 1e-8f) return Quaternion{};
		float inv = 1.0f / m;
		float half = angleDegrees * (3.14159265358979323846f / 360.0f);
		float s = sinf(half);
		return Quaternion{ axisX * inv * s, axisY * inv * s, axisZ * inv * s, cosf(half) };
	}

	// ---- quaternion <-> matrix (rotation only; translation untouched by FromMatrix) --------------
	void FromMatrix(Transform* m, Quaternion* out)
	{
		if (!m || !out) return;
		float trace = m->M11 + m->M22 + m->M33;
		Quaternion q{};
		if (trace > 0.0f) {
			float s = Sqrt(trace + 1.0f) * 2.0f;
			q.w = 0.25f * s;
			q.x = (m->M32 - m->M23) / s;   // row-vector convention
			q.y = (m->M13 - m->M31) / s;
			q.z = (m->M21 - m->M12) / s;
		} else if (m->M11 > m->M22 && m->M11 > m->M33) {
			float s = Sqrt(1.0f + m->M11 - m->M22 - m->M33) * 2.0f;
			q.w = (m->M32 - m->M23) / s;
			q.x = 0.25f * s;
			q.y = (m->M12 + m->M21) / s;
			q.z = (m->M13 + m->M31) / s;
		} else if (m->M22 > m->M33) {
			float s = Sqrt(1.0f + m->M22 - m->M11 - m->M33) * 2.0f;
			q.w = (m->M13 - m->M31) / s;
			q.x = (m->M12 + m->M21) / s;
			q.y = 0.25f * s;
			q.z = (m->M23 + m->M32) / s;
		} else {
			float s = Sqrt(1.0f + m->M33 - m->M11 - m->M22) * 2.0f;
			q.w = (m->M21 - m->M12) / s;
			q.x = (m->M13 + m->M31) / s;
			q.y = (m->M23 + m->M32) / s;
			q.z = 0.25f * s;
		}
		*out = Normalize(q);
	}

	void ToMatrix(Quaternion* in, Transform* out)
	{
		if (!in || !out) return;
		Quaternion q = Normalize(*in);
		float xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
		float xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
		float wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;

		// Row-vector rotation matrix (transpose of the column-vector form).
		out->M11 = 1.0f - 2.0f * (yy + zz); out->M12 = 2.0f * (xy + wz);       out->M13 = 2.0f * (xz - wy);       out->M14 = 0.0f;
		out->M21 = 2.0f * (xy - wz);        out->M22 = 1.0f - 2.0f * (xx + zz); out->M23 = 2.0f * (yz + wx);       out->M24 = 0.0f;
		out->M31 = 2.0f * (xz + wy);        out->M32 = 2.0f * (yz - wx);        out->M33 = 1.0f - 2.0f * (xx + yy); out->M34 = 0.0f;
	}

	// ---- matrix maths ----------------------------------------------------------------------------
	Transform Multiply(const Transform& A, const Transform& B)
	{
		Transform C;
		const float* a = &A.M11;
		const float* b = &B.M11;
		float* c = &C.M11;
		for (int i = 0; i < 4; ++i)
			for (int j = 0; j < 4; ++j)
				c[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j]
				             + a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
		return C;
	}

	bool Invert(const Transform& m, Transform& out)
	{
		// General 4x4 inverse via cofactors (works for rigid and non-rigid).
		const float* a = &m.M11;
		float inv[16];

		inv[0]  =  a[5]*a[10]*a[15] - a[5]*a[11]*a[14] - a[9]*a[6]*a[15] + a[9]*a[7]*a[14] + a[13]*a[6]*a[11] - a[13]*a[7]*a[10];
		inv[4]  = -a[4]*a[10]*a[15] + a[4]*a[11]*a[14] + a[8]*a[6]*a[15] - a[8]*a[7]*a[14] - a[12]*a[6]*a[11] + a[12]*a[7]*a[10];
		inv[8]  =  a[4]*a[9]*a[15]  - a[4]*a[11]*a[13] - a[8]*a[5]*a[15] + a[8]*a[7]*a[13] + a[12]*a[5]*a[11] - a[12]*a[7]*a[9];
		inv[12] = -a[4]*a[9]*a[14]  + a[4]*a[10]*a[13] + a[8]*a[5]*a[14] - a[8]*a[6]*a[13] - a[12]*a[5]*a[10] + a[12]*a[6]*a[9];
		inv[1]  = -a[1]*a[10]*a[15] + a[1]*a[11]*a[14] + a[9]*a[2]*a[15] - a[9]*a[3]*a[14] - a[13]*a[2]*a[11] + a[13]*a[3]*a[10];
		inv[5]  =  a[0]*a[10]*a[15] - a[0]*a[11]*a[14] - a[8]*a[2]*a[15] + a[8]*a[3]*a[14] + a[12]*a[2]*a[11] - a[12]*a[3]*a[10];
		inv[9]  = -a[0]*a[9]*a[15]  + a[0]*a[11]*a[13] + a[8]*a[1]*a[15] - a[8]*a[3]*a[13] - a[12]*a[1]*a[11] + a[12]*a[3]*a[9];
		inv[13] =  a[0]*a[9]*a[14]  - a[0]*a[10]*a[13] - a[8]*a[1]*a[14] + a[8]*a[2]*a[13] + a[12]*a[1]*a[10] - a[12]*a[2]*a[9];
		inv[2]  =  a[1]*a[6]*a[15]  - a[1]*a[7]*a[14]  - a[5]*a[2]*a[15] + a[5]*a[3]*a[14] + a[13]*a[2]*a[7]  - a[13]*a[3]*a[6];
		inv[6]  = -a[0]*a[6]*a[15]  + a[0]*a[7]*a[14]  + a[4]*a[2]*a[15] - a[4]*a[3]*a[14] - a[12]*a[2]*a[7]  + a[12]*a[3]*a[6];
		inv[10] =  a[0]*a[5]*a[15]  - a[0]*a[7]*a[13]  - a[4]*a[1]*a[15] + a[4]*a[3]*a[13] + a[12]*a[1]*a[7]  - a[12]*a[3]*a[5];
		inv[14] = -a[0]*a[5]*a[14]  + a[0]*a[6]*a[13]  + a[4]*a[1]*a[14] - a[4]*a[2]*a[13] - a[12]*a[1]*a[6]  + a[12]*a[2]*a[5];
		inv[3]  = -a[1]*a[6]*a[11]  + a[1]*a[7]*a[10]  + a[5]*a[2]*a[11] - a[5]*a[3]*a[10] - a[9]*a[2]*a[7]   + a[9]*a[3]*a[6];
		inv[7]  =  a[0]*a[6]*a[11]  - a[0]*a[7]*a[10]  - a[4]*a[2]*a[11] + a[4]*a[3]*a[10] + a[8]*a[2]*a[7]   - a[8]*a[3]*a[6];
		inv[11] = -a[0]*a[5]*a[11]  + a[0]*a[7]*a[9]   + a[4]*a[1]*a[11] - a[4]*a[3]*a[9]  - a[8]*a[1]*a[7]   + a[8]*a[3]*a[5];
		inv[15] =  a[0]*a[5]*a[10]  - a[0]*a[6]*a[9]   - a[4]*a[1]*a[10] + a[4]*a[2]*a[9]  + a[8]*a[1]*a[6]   - a[8]*a[2]*a[5];

		float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
		if (fabsf(det) < 1e-12f) return false;
		float invDet = 1.0f / det;
		float* o = &out.M11;
		for (int i = 0; i < 16; ++i) o[i] = inv[i] * invDet;
		return true;
	}

	// ---- transforms applied to vectors ------------------------------------------------------------
	Vector4 operator *(Vector4 v, Transform m)          // row vector: v' = v * M
	{
		return Vector4{
			v.x * m.M11 + v.y * m.M21 + v.z * m.M31 + v.w * m.M41,
			v.x * m.M12 + v.y * m.M22 + v.z * m.M32 + v.w * m.M42,
			v.x * m.M13 + v.y * m.M23 + v.z * m.M33 + v.w * m.M43,
			v.x * m.M14 + v.y * m.M24 + v.z * m.M34 + v.w * m.M44
		};
	}

	Vector4 operator *(Transform m, Vector4 v)          // M * v
	{
		return Vector4{
			m.M11 * v.x + m.M12 * v.y + m.M13 * v.z + m.M14 * v.w,
			m.M21 * v.x + m.M22 * v.y + m.M23 * v.z + m.M24 * v.w,
			m.M31 * v.x + m.M32 * v.y + m.M33 * v.z + m.M34 * v.w,
			m.M41 * v.x + m.M42 * v.y + m.M43 * v.z + m.M44 * v.w
		};
	}

	// ---- quaternion algebra ----------------------------------------------------------------------
	Quaternion operator *(Quaternion a, Quaternion b)
	{
		return Quaternion{
			a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
			a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
			a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
			a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
		};
	}

	Vector3 operator *(Quaternion q, Vector3 v)          // v' = q v q*
	{
		Vector3 u{ q.x, q.y, q.z };
		Vector3 t = Cross(u, v) * 2.0f;
		Vector3 c = Cross(u, t);
		return Vector3{ v.x + q.w * t.x + c.x, v.y + q.w * t.y + c.y, v.z + q.w * t.z + c.z };
	}

	Quaternion operator *(Quaternion q, float s) { return Quaternion{ q.x * s, q.y * s, q.z * s, q.w * s }; }
	Quaternion operator /(Quaternion q, float s) { float i = 1.0f / s; return Quaternion{ q.x * i, q.y * i, q.z * i, q.w * i }; }

	// ---- component operators ----------------------------------------------------------------------
	Vector4 operator +(Vector4 a, Vector4 b) { return Vector4{ a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w }; }
	Vector4 operator +(Vector4 a, Vector3 b) { return Vector4{ a.x + b.x, a.y + b.y, a.z + b.z, a.w }; }
	Vector3 operator +(Vector3 a, Vector3 b) { return Vector3{ a.x + b.x, a.y + b.y, a.z + b.z }; }
	Vector4 operator -(Vector4 a, Vector4 b) { return Vector4{ a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w }; }
	Vector4 operator -(Vector4 a, Vector3 b) { return Vector4{ a.x - b.x, a.y - b.y, a.z - b.z, a.w }; }
	Vector3 operator -(Vector3 a, Vector3 b) { return Vector3{ a.x - b.x, a.y - b.y, a.z - b.z }; }
	Vector3 operator *(Vector3 v, float s)   { return Vector3{ v.x * s, v.y * s, v.z * s }; }
	Vector3 operator *(Vector3 a, Vector3 b) { return Vector3{ a.x * b.x, a.y * b.y, a.z * b.z }; }
	Vector4 operator *(Vector4 v, float s)   { return Vector4{ v.x * s, v.y * s, v.z * s, v.w * s }; }
	Vector3 operator /(Vector3 v, float s)   { float i = 1.0f / s; return Vector3{ v.x * i, v.y * i, v.z * i }; }
	Vector4 operator /(Vector4 v, float s)   { float i = 1.0f / s; return Vector4{ v.x * i, v.y * i, v.z * i, v.w * i }; }
}
