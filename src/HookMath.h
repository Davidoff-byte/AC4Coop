#pragma once

#ifndef PI
#define PI 3.14159265
#endif

#include <Windows.h>

namespace HookMath
{
	struct Plane {
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
		float w = 0.0f;
	};

	struct Rect
	{
		float m_X = 0.0f;
		float m_Y = 0.0f;
		float m_Width = 0.0f;
		float m_Height = 0.0f;
	};

	struct Vector2 {
		float x = 0.0f;
		float y = 0.0f;
	};

	struct Vector3 {
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
	};

	struct __declspec(align(16)) Vector4 {
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
		float w = 0.0f;
	};

	struct __declspec(align(16)) Quaternion {
		float x = 0.0f;
		float y = 0.0f;
		float z = 0.0f;
		float w = 1.0f;
	};

	struct __declspec(align(16)) PosQuat {
		Vector4 m_Pos = {};
		Quaternion m_Quat = {};
	};

	struct __declspec(align(16)) Matrix3x3
	{
		float M11 = 1.0f;
		float M12 = 0.0f;
		float M13 = 0.0f;
		float M21 = 0.0f;
		float M22 = 1.0f;
		float M23 = 0.0f;
		float M31 = 0.0f;
		float M32 = 0.0f;
		float M33 = 1.0f;
	};

	struct __declspec(align(16)) Transform
	{
		float M11 = 1.0f;
		float M12 = 0.0f;
		float M13 = 0.0f;
		float M14 = 0.0f;
		float M21 = 0.0f;
		float M22 = 1.0f;
		float M23 = 0.0f;
		float M24 = 0.0f;
		float M31 = 0.0f;
		float M32 = 0.0f;
		float M33 = 1.0f;
		float M34 = 0.0f;
		float M41 = 0.0f;
		float M42 = 0.0f;
		float M43 = 0.0f;
		float M44 = 1.0f;
	};

	inline Transform Rx90()
	{
		Transform R;
		R.M11 = 1.0f; R.M12 = 0.0f; R.M13 = 0.0f; R.M14 = 0.0f;
		R.M21 = 0.0f; R.M22 = 0.0f; R.M23 = -1.0f; R.M24 = 0.0f;
		R.M31 = 0.0f; R.M32 = 1.0f; R.M33 = 0.0f; R.M34 = 0.0f;
		R.M41 = 0.0f; R.M42 = 0.0f; R.M43 = 0.0f; R.M44 = 1.0f;
		return R;
	}

	inline Transform Ry90()
	{
		Transform R;
		R.M11 = 0.0f; R.M12 = 0.0f; R.M13 = 1.0f; R.M14 = 0.0f;
		R.M21 = 0.0f; R.M22 = 1.0f; R.M23 = 0.0f; R.M24 = 0.0f;
		R.M31 = -1.0f; R.M32 = 0.0f; R.M33 = 0.0f; R.M34 = 0.0f;
		R.M41 = 0.0f; R.M42 = 0.0f; R.M43 = 0.0f; R.M44 = 1.0f;
		return R;
	}

	inline Transform Rz90()
	{
		Transform R;
		R.M11 = 0.0f; R.M12 = -1.0f; R.M13 = 0.0f; R.M14 = 0.0f;
		R.M21 = 1.0f; R.M22 = 0.0f; R.M23 = 0.0f; R.M24 = 0.0f;
		R.M31 = 0.0f; R.M32 = 0.0f; R.M33 = 1.0f; R.M34 = 0.0f;
		R.M41 = 0.0f; R.M42 = 0.0f; R.M43 = 0.0f; R.M44 = 1.0f;
		return R;
	}

	inline Transform RxNeg90()
	{
		Transform R;
		R.M11 = 1.0f; R.M12 = 0.0f; R.M13 = 0.0f; R.M14 = 0.0f;
		R.M21 = 0.0f; R.M22 = 0.0f; R.M23 = 1.0f; R.M24 = 0.0f;
		R.M31 = 0.0f; R.M32 = -1.0f; R.M33 = 0.0f; R.M34 = 0.0f;
		R.M41 = 0.0f; R.M42 = 0.0f; R.M43 = 0.0f; R.M44 = 1.0f;
		return R;
	}

	inline Transform RyNeg90()
	{
		Transform R;
		R.M11 = 0.0f; R.M12 = 0.0f; R.M13 = -1.0f; R.M14 = 0.0f;
		R.M21 = 0.0f; R.M22 = 1.0f; R.M23 = 0.0f; R.M24 = 0.0f;
		R.M31 = 1.0f; R.M32 = 0.0f; R.M33 = 0.0f; R.M34 = 0.0f;
		R.M41 = 0.0f; R.M42 = 0.0f; R.M43 = 0.0f; R.M44 = 1.0f;
		return R;
	}

	inline Transform RzNeg90()
	{
		Transform R;
		R.M11 = 0.0f; R.M12 = 1.0f; R.M13 = 0.0f; R.M14 = 0.0f;
		R.M21 = -1.0f; R.M22 = 0.0f; R.M23 = 0.0f; R.M24 = 0.0f;
		R.M31 = 0.0f; R.M32 = 0.0f; R.M33 = 1.0f; R.M34 = 0.0f;
		R.M41 = 0.0f; R.M42 = 0.0f; R.M43 = 0.0f; R.M44 = 1.0f;
		return R;
	}

	Quaternion __fastcall EulerAnglesToQuaternion(float Heading, float Attitude, float Bank);
	float __fastcall QuatDotProduct(Quaternion qa, Quaternion qb);
	float __fastcall lerp(float a, float b, double t);
	Quaternion __fastcall slerp(Quaternion qa, Quaternion qb, double t);
	Vector3 __fastcall vlerp(Vector3 va, Vector3 vb, double t);
	Vector4 __fastcall vlerp(Vector4 va, Vector4 vb, double t);
	Vector3 __fastcall QuaternionToEuler(Quaternion q);
	float Modulo(Vector3 in);
	float Modulo(Vector4 in);
	float Modulo(Quaternion in);
	Vector3 Normalize(Vector3 in);
	Vector4 Normalize(Vector4 in);
	Quaternion Normalize(Quaternion in);
	Vector3 Cross(Vector3 a, Vector3 b);
	Vector4 Cross(Vector4 a, Vector4 b);
	float Dot(Vector3 a, Vector3 b);
	float Dot(Vector4 a, Vector4 b);
	Quaternion MakeRotation(float angleDegrees, float axisX, float axisY, float axisZ);
	void FromMatrix(Transform* transform, Quaternion* out);
	void ToMatrix(Quaternion* in, Transform* out);
	bool Invert(const Transform& m, Transform& out);
	Transform Multiply(const Transform& A, const Transform& B);
	Vector4 operator *(Transform transform, Vector4 vector);
	Vector4 operator *(Vector4 vector, Transform transform);
	Vector3 operator *(Quaternion quat, Vector3 vec);
	Vector4 operator +(Vector4 v, Vector4 vector);
	Vector4 operator +(Vector4 v, Vector3 vector);
	Vector3 operator +(Vector3 v, Vector3 vector);
	Vector3 operator -(Vector3 v, Vector3 vector);
	Vector4 operator -(Vector4 v, Vector4 vector);
	Vector3 operator *(Vector3 v, float scalar);
	Vector3 operator *(Vector3 v, Vector3 in);
	Vector4 operator *(Vector4 v, float scalar);
	Vector3 operator /(Vector3 v, float scalar);
	Vector4 operator /(Vector4 v, float scalar);
	Vector4 operator -(Vector4 v, Vector3 vector);
	Quaternion operator *(Quaternion v, float scalar);
	Quaternion operator /(Quaternion v, float scalar);
	Quaternion operator *(Quaternion in, Quaternion other);
}