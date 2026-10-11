#include "HookMath.h"
#include <math.h>
#include "Hook.h"

namespace HookMath
{
	float __fastcall Sign(float val)
	{
		if (val > 0.0f)
		{
			return 1.0f;
		}

		return -1.0f;
	}

	Quaternion __fastcall EulerAnglesToQuaternion(float Heading, float Attitude, float Bank)
	{
		float c1 = (float)cos((Heading / 2.0f) * PI / 180.0f);
		float c2 = (float)cos((Attitude / 2.0f) * PI / 180.0f);
		float c3 = (float)cos((Bank / 2.0f) * PI / 180.0f);
		float s1 = (float)sin((Heading / 2.0f) * PI / 180.0f);
		float s2 = (float)sin((Attitude / 2.0f) * PI / 180.0f);
		float s3 = (float)sin((Bank / 2.0f) * PI / 180.0f);
		Quaternion newQuat = {};
		newQuat.w = c1 * c2 * c3 - s1 * s2 * s3;
		newQuat.x = s1 * s2 * c3 + c1 * c2 * s3;
		newQuat.y = s1 * c2 * c3 + c1 * s2 * s3;
		newQuat.z = c1 * s2 * c3 - s1 * c2 * s3;
		return newQuat;
	}

	float __fastcall QuatDotProduct(Quaternion qa, Quaternion qb)
	{
		return qa.w * qb.w + qa.x * qb.x + qa.y * qb.y + qa.z * qb.z;
	}

	float __fastcall lerp(float a, float b, double t)
	{
		return (float)((1.0 - t) * a + b * t);
	}

	Quaternion __fastcall slerp(Quaternion qa, Quaternion qb, double t)
	{
		Quaternion qm = {};
		double cosHalfTheta = QuatDotProduct(qa, qb);

		if (cosHalfTheta < 0.0)
		{
			qa.x = -qa.x;
			qa.y = -qa.y;
			qa.z = -qa.z;
			qa.w = -qa.w;
		}

		cosHalfTheta = qa.w * qb.w + qa.x * qb.x + qa.y * qb.y + qa.z * qb.z;

		if (fabs(cosHalfTheta) >= 1.0) {
			qm.w = qa.w; qm.x = qa.x; qm.y = qa.y; qm.z = qa.z;
			return qm;
		}

		double halfTheta = acos(cosHalfTheta);
		double sinHalfTheta = sqrt(1.0 - cosHalfTheta * cosHalfTheta);

		if (fabs(sinHalfTheta) < 0.001) {
			qm.w = (qa.w * 0.5f + qb.w * 0.5f);
			qm.x = (qa.x * 0.5f + qb.x * 0.5f);
			qm.y = (qa.y * 0.5f + qb.y * 0.5f);
			qm.z = (qa.z * 0.5f + qb.z * 0.5f);
			return qm;
		}

		float ratioA = (float)(sin((1 - t) * halfTheta) / sinHalfTheta);
		float ratioB = (float)(sin(t * halfTheta) / sinHalfTheta);

		qm.w = (qa.w * ratioA + qb.w * ratioB);
		qm.x = (qa.x * ratioA + qb.x * ratioB);
		qm.y = (qa.y * ratioA + qb.y * ratioB);
		qm.z = (qa.z * ratioA + qb.z * ratioB);

		return qm;
	}

	Vector3 __fastcall vlerp(Vector3 va, Vector3 vb, double t)
	{
		Vector3 vm = {};
		vm.x = lerp(va.x, vb.x, t);
		vm.y = lerp(va.y, vb.y, t);
		vm.z = lerp(va.z, vb.z, t);

		return vm;
	}

	Vector4 __fastcall vlerp(Vector4 va, Vector4 vb, double t)
	{
		Vector4 vm = {};
		vm.x = lerp(va.x, vb.x, t);
		vm.y = lerp(va.y, vb.y, t);
		vm.z = lerp(va.z, vb.z, t);
		vm.w = lerp(va.w, vb.w, t);

		return vm;
	}

	Vector3 __fastcall QuaternionToEuler(Quaternion q)
	{
		Vector3 v = {};
		v.x = atan2f(2 * q.y * q.w - 2 * q.x * q.z, 1 - 2 * q.y * q.y - 2 * q.z * q.z);
		v.y = asinf(2 * q.x * q.y + 2 * q.z * q.w);
		v.z = atan2f(2 * q.x * q.w - 2 * q.y * q.z, 1 - 2 * q.x * q.x - 2 * q.z * q.z);

		return v;
	}

	float Modulo(Vector3 in)
	{
		return sqrtf(in.x * in.x + in.y * in.y + in.z * in.z);
	}

	float Modulo(Vector4 in)
	{
		return sqrtf(in.x * in.x + in.y * in.y + in.z * in.z + in.w * in.w);
	}

	float Modulo(Quaternion in)
	{
		return sqrtf(in.x * in.x + in.y * in.y + in.z * in.z + in.w * in.w);
	}

	Vector3 Normalize(Vector3 in)
	{
		return in / Modulo(in);
	}

	Vector4 Normalize(Vector4 in)
	{
		return in / Modulo(in);
	}

	Quaternion Normalize(Quaternion in)
	{
		return in / Modulo(in);
	}

	Vector3 Cross(Vector3 a, Vector3 b)
	{
		Vector3 result = {
			a.y * b.z - a.z * b.y,
			a.z * b.x - a.x * b.z,
			a.x * b.y - a.y * b.x,
		};

		return result;
	}

	Vector4 Cross(Vector4 a, Vector4 b)
	{
		Vector4 result = {
			a.y * b.z - a.z * b.y,
			a.z * b.x - a.x * b.z,
			a.x * b.y - a.y * b.x,
			a.w * b.w
		};

		return result;
	}

	float Dot(Vector3 a, Vector3 b)
	{
		return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
	}

	float Dot(Vector4 a, Vector4 b)
	{
		return (a.x * b.x) + (a.y * b.y) + (a.z * b.z) + (a.w * b.w);
	}

	Quaternion MakeRotation(float angleDegrees, float axisX, float axisY, float axisZ)
	{
		float angleRad = angleDegrees * (float)PI / 180.0f;
		float halfAngle = angleRad / 2.0f;
		float s = (float)sin(halfAngle);
		return {
			s * axisX,
			s * axisY,
			s * axisZ,
			(float)cos(halfAngle)
		};
	}

	void FromMatrix(Transform* transform, Quaternion* out)
	{
		PosQuat q = {};
		Hook::CallClassMethod<Transform*, PosQuat*, Transform*>(0x0042AC30, &q, transform);
		out->x = q.m_Quat.x;
		out->y = q.m_Quat.y;
		out->z = q.m_Quat.z;
		out->w = q.m_Quat.w;
	}

	void ToMatrix(Quaternion* in, Transform* out)
	{
		Hook::CallClassMethod<Quaternion*, Quaternion*, Transform*>(0x00430E60, in, out);
	}

	bool Invert(const Transform& m, Transform& out)
	{
		float det;

		float a0 = m.M11 * m.M22 - m.M12 * m.M21;
		float a1 = m.M11 * m.M23 - m.M13 * m.M21;
		float a2 = m.M11 * m.M24 - m.M14 * m.M21;
		float a3 = m.M12 * m.M23 - m.M13 * m.M22;
		float a4 = m.M12 * m.M24 - m.M14 * m.M22;
		float a5 = m.M13 * m.M24 - m.M14 * m.M23;

		float b0 = m.M31 * m.M42 - m.M32 * m.M41;
		float b1 = m.M31 * m.M43 - m.M33 * m.M41;
		float b2 = m.M31 * m.M44 - m.M34 * m.M41;
		float b3 = m.M32 * m.M43 - m.M33 * m.M42;
		float b4 = m.M32 * m.M44 - m.M34 * m.M42;
		float b5 = m.M33 * m.M44 - m.M34 * m.M43;

		det = a0 * b5 - a1 * b4 + a2 * b3 + a3 * b2 - a4 * b1 + a5 * b0;

		if (fabsf(det) < 1e-6f)
			return false;

		float invDet = 1.0f / det;

		out.M11 = (+m.M22 * b5 - m.M23 * b4 + m.M24 * b3) * invDet;
		out.M12 = (-m.M12 * b5 + m.M13 * b4 - m.M14 * b3) * invDet;
		out.M13 = (+m.M42 * a5 - m.M43 * a4 + m.M44 * a3) * invDet;
		out.M14 = (-m.M32 * a5 + m.M33 * a4 - m.M34 * a3) * invDet;

		out.M21 = (-m.M21 * b5 + m.M23 * b2 - m.M24 * b1) * invDet;
		out.M22 = (+m.M11 * b5 - m.M13 * b2 + m.M14 * b1) * invDet;
		out.M23 = (-m.M41 * a5 + m.M43 * a2 - m.M44 * a1) * invDet;
		out.M24 = (+m.M31 * a5 - m.M33 * a2 + m.M34 * a1) * invDet;

		out.M31 = (+m.M21 * b4 - m.M22 * b2 + m.M24 * b0) * invDet;
		out.M32 = (-m.M11 * b4 + m.M12 * b2 - m.M14 * b0) * invDet;
		out.M33 = (+m.M41 * a4 - m.M42 * a2 + m.M44 * a0) * invDet;
		out.M34 = (-m.M31 * a4 + m.M32 * a2 - m.M34 * a0) * invDet;

		out.M41 = (-m.M21 * b3 + m.M22 * b1 - m.M23 * b0) * invDet;
		out.M42 = (+m.M11 * b3 - m.M12 * b1 + m.M13 * b0) * invDet;
		out.M43 = (-m.M41 * a3 + m.M42 * a1 - m.M43 * a0) * invDet;
		out.M44 = (+m.M31 * a3 - m.M32 * a1 + m.M33 * a0) * invDet;

		return true;
	}

	Transform Multiply(const Transform& A, const Transform& B)
	{
		Transform C;

		C.M11 = A.M11 * B.M11 + A.M12 * B.M21 + A.M13 * B.M31 + A.M14 * B.M41;
		C.M12 = A.M11 * B.M12 + A.M12 * B.M22 + A.M13 * B.M32 + A.M14 * B.M42;
		C.M13 = A.M11 * B.M13 + A.M12 * B.M23 + A.M13 * B.M33 + A.M14 * B.M43;
		C.M14 = A.M11 * B.M14 + A.M12 * B.M24 + A.M13 * B.M34 + A.M14 * B.M44;

		C.M21 = A.M21 * B.M11 + A.M22 * B.M21 + A.M23 * B.M31 + A.M24 * B.M41;
		C.M22 = A.M21 * B.M12 + A.M22 * B.M22 + A.M23 * B.M32 + A.M24 * B.M42;
		C.M23 = A.M21 * B.M13 + A.M22 * B.M23 + A.M23 * B.M33 + A.M24 * B.M43;
		C.M24 = A.M21 * B.M14 + A.M22 * B.M24 + A.M23 * B.M34 + A.M24 * B.M44;

		C.M31 = A.M31 * B.M11 + A.M32 * B.M21 + A.M33 * B.M31 + A.M34 * B.M41;
		C.M32 = A.M31 * B.M12 + A.M32 * B.M22 + A.M33 * B.M32 + A.M34 * B.M42;
		C.M33 = A.M31 * B.M13 + A.M32 * B.M23 + A.M33 * B.M33 + A.M34 * B.M43;
		C.M34 = A.M31 * B.M14 + A.M32 * B.M24 + A.M33 * B.M34 + A.M34 * B.M44;

		C.M41 = A.M41 * B.M11 + A.M42 * B.M21 + A.M43 * B.M31 + A.M44 * B.M41;
		C.M42 = A.M41 * B.M12 + A.M42 * B.M22 + A.M43 * B.M32 + A.M44 * B.M42;
		C.M43 = A.M41 * B.M13 + A.M42 * B.M23 + A.M43 * B.M33 + A.M44 * B.M43;
		C.M44 = A.M41 * B.M14 + A.M42 * B.M24 + A.M43 * B.M34 + A.M44 * B.M44;

		return C;
	}

	Vector4 operator *(Transform transform, Vector4 vector) {
		Vector4 result = {};
		result.x = transform.M11 * vector.x + transform.M12 * vector.y + transform.M13 * vector.z + transform.M14 * vector.w;
		result.y = transform.M21 * vector.x + transform.M22 * vector.y + transform.M23 * vector.z + transform.M24 * vector.w;
		result.z = transform.M31 * vector.x + transform.M32 * vector.y + transform.M33 * vector.z + transform.M34 * vector.w;
		result.w = transform.M41 * vector.x + transform.M42 * vector.y + transform.M43 * vector.z + transform.M44 * vector.w;

		return result;
	}

	Vector4 operator *(Vector4 vector, Transform transform) {
		Vector4 result = {};
		result.x = vector.x * transform.M11 + vector.y * transform.M21 + vector.z * transform.M31 + vector.w * transform.M41;
		result.y = vector.x * transform.M12 + vector.y * transform.M22 + vector.z * transform.M32 + vector.w * transform.M42;
		result.z = vector.x * transform.M13 + vector.y * transform.M23 + vector.z * transform.M33 + vector.w * transform.M43;
		result.w = vector.x * transform.M14 + vector.y * transform.M24 + vector.z * transform.M34 + vector.w * transform.M44;

		return result;
	}

	Vector4 operator +(Vector4 v, Vector4 vector) {
		Vector4 result = {};
		result.x = v.x + vector.x;
		result.y = v.y + vector.y;
		result.z = v.z + vector.z;
		result.w = v.w + vector.w;

		return result;
	}

	Vector3 operator +(Vector3 v, Vector3 vector) {
		Vector3 result = {};
		result.x = v.x + vector.x;
		result.y = v.y + vector.y;
		result.z = v.z + vector.z;

		return result;
	}

	Vector3 operator -(Vector3 v, Vector3 vector) {
		Vector3 result = {};
		result.x = v.x - vector.x;
		result.y = v.y - vector.y;
		result.z = v.z - vector.z;

		return result;
	}

	Vector4 operator +(Vector4 v, Vector3 vector) {
		Vector4 result = {};
		result.x = v.x + vector.x;
		result.y = v.y + vector.y;
		result.z = v.z + vector.z;
		result.w = v.w + 0.0f;

		return result;
	}

	Vector4 operator -(Vector4 v, Vector4 vector) {
		Vector4 result = {};
		result.x = v.x - vector.x;
		result.y = v.y - vector.y;
		result.z = v.z - vector.z;
		result.w = v.w - vector.w;

		return result;
	}

	Vector4 operator -(Vector4 v, Vector3 vector) {
		Vector4 result = {};
		result.x = v.x - vector.x;
		result.y = v.y - vector.y;
		result.z = v.z - vector.z;

		return result;
	}

	Vector3 operator *(Vector3 v, float scalar) {
		Vector3 result = {};
		result.x = v.x * scalar;
		result.y = v.y * scalar;
		result.z = v.z * scalar;

		return result;
	}

	Vector3 operator *(Vector3 v, Vector3 in) {
		Vector3 result = {};
		result.x = v.x * in.x;
		result.y = v.y * in.y;
		result.z = v.z * in.z;

		return result;
	}

	Vector4 operator *(Vector4 v, float scalar) {
		Vector4 result = {};
		result.x = v.x * scalar;
		result.y = v.y * scalar;
		result.z = v.z * scalar;
		result.w = v.w * scalar;

		return result;
	}

	Vector3 operator /(Vector3 v, float scalar) {
		Vector3 result = {};
		result.x = v.x / scalar;
		result.y = v.y / scalar;
		result.z = v.z / scalar;

		return result;
	}

	Vector4 operator /(Vector4 v, float scalar) {
		Vector4 result = {};
		result.x = v.x / scalar;
		result.y = v.y / scalar;
		result.z = v.z / scalar;
		result.w = v.w / scalar;

		return result;
	}

	Quaternion operator *(Quaternion v, float scalar) {
		Quaternion result = {};
		result.x = v.x * scalar;
		result.y = v.y * scalar;
		result.z = v.z * scalar;
		result.w = v.w * scalar;

		return result;
	}

	Quaternion operator /(Quaternion v, float scalar) {
		Quaternion result = {};
		result.x = v.x / scalar;
		result.y = v.y / scalar;
		result.z = v.z / scalar;
		result.w = v.w / scalar;

		return result;
	}

	Quaternion operator *(Quaternion in, Quaternion other) {
		Quaternion result = {};
		result.x = in.w * other.x + in.x * other.w + in.y * other.z - in.z * other.y;
		result.y = in.w * other.y - in.x * other.z + in.y * other.w + in.z * other.x;
		result.z = in.w * other.z + in.x * other.y - in.y * other.x + in.z * other.w;
		result.w = in.w * other.w - in.x * other.x - in.y * other.y - in.z * other.z;

		return result;
	}

	Vector3 operator *(Quaternion quat, Vector3 vec)
	{
		float num = quat.x * 2.0f;
		float num2 = quat.y * 2.0f;
		float num3 = quat.z * 2.0f;
		float num4 = quat.x * num;
		float num5 = quat.y * num2;
		float num6 = quat.z * num3;
		float num7 = quat.x * num2;
		float num8 = quat.x * num3;
		float num9 = quat.y * num3;
		float num10 = quat.w * num;
		float num11 = quat.w * num2;
		float num12 = quat.w * num3;
		Vector3 result = {};
		result.x = (1.0f - (num5 + num6)) * vec.x + (num7 - num12) * vec.y + (num8 + num11) * vec.z;
		result.y = (num7 + num12) * vec.x + (1.0f - (num4 + num6)) * vec.y + (num9 - num10) * vec.z;
		result.z = (num8 - num11) * vec.x + (num9 + num10) * vec.y + (1.0f - (num4 + num5)) * vec.z;

		return result;
	}
}