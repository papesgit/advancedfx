#pragma once

#include "AfxRefCounted.h"
#include "AfxMath.h"

#include <list>

using namespace Afx;
using namespace Afx::Math;

struct CamPathValue
{
    double X;
    double Y;
    double Z;

    Quaternion R;

    double Fov;

    bool Selected;

    // Per-key tangents and modes (for Hermite interpolation)
    double TxIn, TxOut; unsigned char TxModeIn, TxModeOut;
    double TyIn, TyOut; unsigned char TyModeIn, TyModeOut;
    double TzIn, TzOut; unsigned char TzModeIn, TzModeOut;
    double TfovIn, TfovOut; unsigned char TfovModeIn, TfovModeOut;

	CamPathValue();

	CamPathValue(double x, double y, double z, double pitch, double yaw, double roll, double fov);

	CamPathValue(double x, double y, double z, double q_w, double q_x, double q_y, double q_z, double fov, bool selected);

};

struct CamPathIterator
{
public:
	CInterpolationMap<CamPathValue>::const_iterator wrapped;

	CamPathIterator(CInterpolationMap<CamPathValue>::const_iterator & it);

	double GetTime() const;

	CamPathValue GetValue() const;

	CamPathIterator& operator ++ ();

	bool operator == (CamPathIterator const &it) const;

	bool operator != (CamPathIterator const &it) const;

};

typedef void (*CamPathChanged)(void * pUserData);

class CamPath
{
public:
    enum DoubleInterp {
        DI_DEFAULT = 0,
        DI_LINEAR = 1,
        DI_CUBIC = 2,
        DI_CUSTOM = 3,
        _DI_COUNT = 4
    };

	enum QuaternionInterp {
		QI_DEFAULT = 0,
		QI_SLINEAR = 1,
		QI_SCUBIC = 2,
		_QI_COUNT = 3,
	};

	static bool DoubleInterp_FromString(char const * value, DoubleInterp & outValue);
	static char const * DoubleInterp_ToString(DoubleInterp value);

    static bool QuaternionInterp_FromString(char const * value, QuaternionInterp & outValue);
    static char const * QuaternionInterp_ToString(QuaternionInterp value);

    // Tangent editing support (for custom Hermite interpolation)
    enum TangentMode : unsigned char {
        TM_AUTO = 0,
        TM_FLAT = 1,
        TM_LINEAR = 2,
        TM_FREE = 3
    };

    static bool TangentMode_FromString(char const* value, unsigned char& outValue);
    static char const* TangentMode_ToString(unsigned char value);

	CamPath();
	
	~CamPath();

	void Enabled_set(bool enable);
	bool Enabled_get(void) const;

	bool GetHold(void) const;
	void SetHold(bool value);

	void PositionInterpMethod_set(DoubleInterp value);
	DoubleInterp PositionInterpMethod_get(void) const;

	void RotationInterpMethod_set(QuaternionInterp value);
	QuaternionInterp RotationInterpMethod_get(void) const;

	void FovInterpMethod_set(DoubleInterp value);
	DoubleInterp FovInterpMethod_get(void) const;

	void Add(double time, const CamPathValue & value);

	void Remove(double time);
	void Clear();

	size_t GetSize() const;
	CamPathIterator GetBegin();
	CamPathIterator GetEnd();
	double GetDuration() const;

	/// <remarks>Must not be called if GetSize is less than 1!</remarks>
	double GetLowerBound() const;

	/// <remarks>Must not be called if GetSize is less than 1!</remarks>
	double GetUpperBound() const;

	bool CanEval(void) const;

	/// <remarks>
	/// Must not be called if CanEval() returns false!<br />
	/// </remarks>
	CamPathValue Eval(double t);

	bool Save(wchar_t const * fileName);
	bool Load(wchar_t const * fileName);
	
	/// <remarks>In the current implementation if points happen to fall on the same time value, then the last point's value will be used (no interpolation).</remarks>
	/// <param name="relative">If t is an relative offset (true), or absolute value (false).</param>
	void SetStart(double t, bool relative = false);
	
	/// <remarks>In the current implementation if points happen to fall on the same time value, then the last point's value will be used (no interpolation).
	/// Setting duration for a path with less than 2 points will do nothing.</remarks>
	void SetDuration(double t);

	void SetPosition(double x, double y, double z, bool setX = true, bool setY = true, bool setZ = true);

	void SetAngles(double yPitch, double zYaw, double xRoll, bool setY = true, bool setZ = true, bool setX = true);

    void SetFov(double fov);

    // Custom tangent editing APIs (apply to selected keys if any, otherwise all)
    enum Channel { CH_X = 0, CH_Y = 1, CH_Z = 2, CH_FOV = 3 };
    void SetTangent(Channel ch, bool setIn, bool setOut, double slopeIn, double slopeOut);
    void SetTangentMode(Channel ch, bool setIn, bool setOut, unsigned char mode);

	void Rotate(double yPitch, double zYaw, double xRoll);

	void AnchorTransform(double anchorX, double anchorY, double anchorZ, double anchorYPitch, double anchorZYaw, double anchorXRoll, double destX, double destY, double destZ, double destYPitch, double destZYaw, double destXRoll);

	size_t SelectAll();

	void SelectNone();

	size_t SelectInvert();

	/// <summary>Adds a range of key frames to the selection.</summary>
	/// <param name="min">Index of first keyframe to add to selection.</param>
	/// <param name="max">Index of last keyframe to add to selection.</param>
	/// <returns>Number of selected keyframes.</returns>
	size_t SelectAdd(size_t min, size_t max);

	/// <summary>Adds a range of key frames to the selection.</summary>
	/// <param name="min">Lower bound to start adding selection at.</param>
	/// <param name="count">Number of keyframes to select.</param>
	/// <returns>Number of selected keyframes.</returns>
	size_t SelectAdd(double min, size_t count);

	/// <summary>Adds a range of key frames to the selection.</summary>
	/// <param name="min">Lower bound to start adding selection at.</param>
	/// <param name="max">Upper bound to end adding selection at.</param>
	/// <returns>Number of selected keyframes.</returns>
	size_t SelectAdd(double min, double max);
	
	void OnChangedAdd(CamPathChanged pCamPathChanged, void * pUserData);

	void OnChangedRemove(CamPathChanged pCamPathChanged, void * pUserData);

	void SetOffset(double value);

	double GetOffset() const;

private:
	struct CamPathChangedData {
		CamPathChanged pFn;
		void * pUserData;

		CamPathChangedData(CamPathChanged pFn, void * pUserData)
		: pFn(pFn), pUserData(pUserData) {

		}

		void Notify() {
			pFn(pUserData);
		}

		bool operator==(const CamPathChangedData& other) const {
			return this->pFn == other.pFn && this->pUserData == other.pUserData;
		}
	};

	std::list<struct CamPathChangedData> m_OnChanged;
	std::list<struct CamPathChangedData>::iterator m_OnChangedIt;

	static double XSelector(CamPathValue const & value)
	{
		return value.X;
	}

	static double YSelector(CamPathValue const & value)
	{
		return value.Y;
	}

	static double ZSelector(CamPathValue const & value)
	{
		return value.Z;
	}

	static Quaternion RSelector(CamPathValue const & value)
	{
		return value.R;
	}

	static double FovSelector(CamPathValue const & value)
	{
		return value.Fov;
	}

    static bool SelectedSelector(CamPathValue const & value)
    {
        return value.Selected;
    }

    // Tangent selectors for Hermite interpolation (double channels)
    static double XTanInSelector(CamPathValue const& v) { return v.TxIn; }
    static double XTanOutSelector(CamPathValue const& v) { return v.TxOut; }
    static unsigned char XTanModeInSelector(CamPathValue const& v) { return v.TxModeIn; }
    static unsigned char XTanModeOutSelector(CamPathValue const& v) { return v.TxModeOut; }

    static double YTanInSelector(CamPathValue const& v) { return v.TyIn; }
    static double YTanOutSelector(CamPathValue const& v) { return v.TyOut; }
    static unsigned char YTanModeInSelector(CamPathValue const& v) { return v.TyModeIn; }
    static unsigned char YTanModeOutSelector(CamPathValue const& v) { return v.TyModeOut; }

    static double ZTanInSelector(CamPathValue const& v) { return v.TzIn; }
    static double ZTanOutSelector(CamPathValue const& v) { return v.TzOut; }
    static unsigned char ZTanModeInSelector(CamPathValue const& v) { return v.TzModeIn; }
    static unsigned char ZTanModeOutSelector(CamPathValue const& v) { return v.TzModeOut; }

    static double FovTanInSelector(CamPathValue const& v) { return v.TfovIn; }
    static double FovTanOutSelector(CamPathValue const& v) { return v.TfovOut; }
    static unsigned char FovTanModeInSelector(CamPathValue const& v) { return v.TfovModeIn; }
    static unsigned char FovTanModeOutSelector(CamPathValue const& v) { return v.TfovModeOut; }

	bool m_Enabled;
	bool m_Hold = false;
	DoubleInterp m_PositionInterpMethod;
	QuaternionInterp m_RotationInterpMethod;
	DoubleInterp m_FovInterpMethod;
	double m_Offset;
	
	CInterpolationMap<CamPathValue> m_Map;

	CInterpolationMapView<CamPathValue, double> m_XView;
	CInterpolationMapView<CamPathValue, double> m_YView;
	CInterpolationMapView<CamPathValue, double> m_ZView;
	CInterpolationMapView<CamPathValue, Quaternion> m_RView;
	CInterpolationMapView<CamPathValue, double> m_FovView;
	CInterpolationMapView<CamPathValue, bool> m_SelectedView;

	CInterpolation<double> * m_XInterp;
	CInterpolation<double> * m_YInterp;
	CInterpolation<double> * m_ZInterp;
	CInterpolation<Quaternion> * m_RInterp;
	CInterpolation<double> * m_FovInterp;
	CInterpolation<bool> * m_SelectedInterp;

	void Changed();
	void CopyMap(CInterpolationMap<CamPathValue> & dst, CInterpolationMap<CamPathValue> & src);

	void DoInterpolationMapChangedAll(void);
};
