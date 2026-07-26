#include "stdafx.h"

#include "CamPath.h"

#include "../deps/release/rapidxml/rapidxml.hpp"
#include "../deps/release/rapidxml/rapidxml_print.hpp"
#include <iterator>
#include <stdio.h>
#include <fstream>
#include <algorithm>
#include <cfloat>
#include <set>

#define _USE_MATH_DEFINES
#include <math.h>

#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif

bool CamPath::DoubleInterp_FromString(char const * value, DoubleInterp & outValue)
{
	if(!_stricmp(value,"default"))
	{
		outValue = DI_DEFAULT;
		return true;
	}
	else
	if(!_stricmp(value,"linear"))
	{
		outValue = DI_LINEAR;
		return true;
	}
	else
	if(!_stricmp(value,"cubic"))
	{
		outValue = DI_CUBIC;
		return true;
	}

	return false;
}

char const * CamPath::DoubleInterp_ToString(DoubleInterp value)
{
	switch(value)
	{
	case DI_DEFAULT:
		return "default";
	case DI_LINEAR:
		return "linear";
	case DI_CUBIC:
		return "cubic";
	}

	return "[unkown]";
}

bool CamPath::QuaternionInterp_FromString(char const * value, QuaternionInterp & outValue)
{
	if(!_stricmp(value,"default"))
	{
		outValue = QI_DEFAULT;
		return true;
	}
	else
	if(!_stricmp(value,"sLinear"))
	{
		outValue = QI_SLINEAR;
		return true;
	}
	else
	if(!_stricmp(value,"sCubic"))
	{
		outValue = QI_SCUBIC;
		return true;
	}

	return false;
}

char const * CamPath::QuaternionInterp_ToString(QuaternionInterp value)
{
	switch(value)
	{
	case QI_DEFAULT:
		return "default";
	case QI_SLINEAR:
		return "sLinear";
	case QI_SCUBIC:
		return "sCubic";
	}

	return "[unkown]";
}

CamPathValue::CamPathValue()
: X(0.0), Y(0.0), Z(0.0), R(), Fov(90.0), Selected(false)
, HasDof(false), DofEnabled(false), DofNearBlurry(-100.0), DofNearCrisp(0.0)
, DofFarCrisp(180.0), DofFarBlurry(2000.0), DofMaxBlurSize(5.0), DofRadiusScale(0.25)
{
}

CamPathValue::CamPathValue(double x, double y, double z, double pitch, double yaw, double roll, double fov)
: X(x)
, Y(y)
, Z(z)
, R(Quaternion::FromQREulerAngles(QREulerAngles::FromQEulerAngles(QEulerAngles(pitch,yaw,roll))))
, Fov(fov)
, Selected(false)
, HasDof(false), DofEnabled(false), DofNearBlurry(-100.0), DofNearCrisp(0.0)
, DofFarCrisp(180.0), DofFarBlurry(2000.0), DofMaxBlurSize(5.0), DofRadiusScale(0.25)
{
}

CamPathValue::CamPathValue(double x, double y, double z, double q_w, double q_x, double q_y, double q_z, double fov, bool selected)
: X(x), Y(y), Z(z), R(Quaternion(q_w,q_x,q_y,q_z)), Fov(fov), Selected(selected)
, HasDof(false), DofEnabled(false), DofNearBlurry(-100.0), DofNearCrisp(0.0)
, DofFarCrisp(180.0), DofFarBlurry(2000.0), DofMaxBlurSize(5.0), DofRadiusScale(0.25) {
}

CamPathIterator::CamPathIterator(CInterpolationMap<CamPathValue>::const_iterator & it) : wrapped(it)
{
}

double CamPathIterator::GetTime() const
{
	return wrapped->first;
}

CamPathValue CamPathIterator::GetValue() const
{
	return wrapped->second;
}

CamPathIterator& CamPathIterator::operator ++ ()
{
	wrapped++;
	return *this;
}

bool CamPathIterator::operator == (CamPathIterator const &it) const
{
	return wrapped == it.wrapped;
}

bool CamPathIterator::operator != (CamPathIterator const &it) const
{
	return !(*this == it);
}

static double CurveBezier(double p0, double p1, double p2, double p3, double u)
{
	double inv = 1.0 - u;
	return inv * inv * inv * p0 + 3.0 * inv * inv * u * p1 + 3.0 * inv * u * u * p2 + u * u * u * p3;
}

double CamPath::CurveChannel::Eval(double time) const
{
	if(Keys.empty()) return 0.0;
	if(1 == Keys.size() || time <= Keys.front().Time) return Keys.front().Value;
	if(time >= Keys.back().Time) return Keys.back().Value;

	size_t index = 0;
	while(index + 1 < Keys.size() && Keys[index + 1].Time < time) ++index;
	CurveKey const & a = Keys[index];
	CurveKey const & b = Keys[index + 1];
	double dt = std::max(1e-9, b.Time - a.Time);
	double u = std::max(0.0, std::min(1.0, (time - a.Time) / dt));
	if(CI_CONSTANT == a.Interpolation) return a.Value;
	if(CI_LINEAR == a.Interpolation) return a.Value + (b.Value - a.Value) * u;

	if(a.Weighted || b.Weighted)
	{
		double outWeight = std::max(0.001, std::min(dt * 0.999, a.OutWeight));
		double inWeight = std::max(0.001, std::min(dt * 0.999, b.InWeight));
		double t0 = a.Time, t1 = a.Time + outWeight, t2 = b.Time - inWeight, t3 = b.Time;
		double lo = 0.0, hi = 1.0;
		for(int i = 0; i < 18; ++i)
		{
			double candidate = (lo + hi) * 0.5;
			if(CurveBezier(t0, t1, t2, t3, candidate) < time) lo = candidate; else hi = candidate;
		}
		u = (lo + hi) * 0.5;
		return CurveBezier(a.Value, a.Value + a.OutTangent * outWeight,
			b.Value - b.InTangent * inWeight, b.Value, u);
	}

	double u2 = u * u, u3 = u2 * u;
	return (2.0 * u3 - 3.0 * u2 + 1.0) * a.Value
		+ (u3 - 2.0 * u2 + u) * dt * a.OutTangent
		+ (-2.0 * u3 + 3.0 * u2) * b.Value
		+ (u3 - u2) * dt * b.InTangent;
}

// CamPath /////////////////////////////////////////////////////////////////////

CamPath::CamPath()
: m_Offset(0)
, m_Enabled(false)
, m_PositionInterpMethod(DI_DEFAULT)
, m_RotationInterpMethod(QI_DEFAULT)
, m_FovInterpMethod(DI_DEFAULT)
, m_XView(&m_Map, XSelector)
, m_YView(&m_Map, YSelector)
, m_ZView(&m_Map, ZSelector)
, m_RView(&m_Map, RSelector)
, m_FovView(&m_Map, FovSelector)
, m_SelectedView(&m_Map, SelectedSelector)
{
	m_OnChangedIt = m_OnChanged.end();

	m_XInterp = new CCubicDoubleInterpolation<CamPathValue>(&m_XView);
	m_YInterp = new CCubicDoubleInterpolation<CamPathValue>(&m_YView);
	m_ZInterp = new CCubicDoubleInterpolation<CamPathValue>(&m_ZView);
	m_RInterp = new CSCubicQuaternionInterpolation<CamPathValue>(&m_RView);
	m_FovInterp = new CCubicDoubleInterpolation<CamPathValue>(&m_FovView);
	m_SelectedInterp = new CBoolAndInterpolation<CamPathValue>(&m_SelectedView);
}

CamPath::~CamPath()
{
	m_Map.clear();

	delete m_SelectedInterp;
	delete m_FovInterp;
	delete m_RInterp;
	delete m_ZInterp;
	delete m_YInterp;
	delete m_XInterp;
}

void CamPath::DoInterpolationMapChangedAll(void)
{
	m_XInterp->InterpolationMapChanged();
	m_YInterp->InterpolationMapChanged();
	m_ZInterp->InterpolationMapChanged();
	m_RInterp->InterpolationMapChanged();
	m_FovInterp->InterpolationMapChanged();
	m_SelectedInterp->InterpolationMapChanged();
}

void CamPath::Enabled_set(bool enable)
{
	m_Enabled = enable;
}

bool CamPath::Enabled_get(void) const
{
	return m_Enabled;
}

bool CamPath::GetHold(void) const
{
	return m_Hold;
}

void CamPath::SetHold(bool value)
{
	m_Hold = value;
}

void CamPath::PositionInterpMethod_set(DoubleInterp value)
{
	delete m_XInterp;
	delete m_YInterp;
	delete m_ZInterp;

	m_PositionInterpMethod = value;

	switch(value)
	{
	case DI_LINEAR:
		m_XInterp = new CLinearDoubleInterpolation<CamPathValue>(&m_XView);
		m_YInterp = new CLinearDoubleInterpolation<CamPathValue>(&m_YView);
		m_ZInterp = new CLinearDoubleInterpolation<CamPathValue>(&m_ZView);
		break;
	default:
		m_XInterp = new CCubicDoubleInterpolation<CamPathValue>(&m_XView);
		m_YInterp = new CCubicDoubleInterpolation<CamPathValue>(&m_YView);
		m_ZInterp = new CCubicDoubleInterpolation<CamPathValue>(&m_ZView);
		break;
	}

	Changed();
}

CamPath::DoubleInterp CamPath::PositionInterpMethod_get(void) const
{
	return m_PositionInterpMethod;
}

void CamPath::RotationInterpMethod_set(QuaternionInterp value)
{
	delete m_RInterp;

	m_RotationInterpMethod = value;

	switch(value)
	{
	case QI_SLINEAR:
		m_RInterp = new CSLinearQuaternionInterpolation<CamPathValue>(&m_RView);
		break;
	default:
		m_RInterp = new CSCubicQuaternionInterpolation<CamPathValue>(&m_RView);
		break;
	}

	Changed();
}

CamPath::QuaternionInterp CamPath::RotationInterpMethod_get(void) const
{
	return m_RotationInterpMethod;
}

void CamPath::FovInterpMethod_set(DoubleInterp value)
{
	delete m_FovInterp;

	m_FovInterpMethod = value;

	switch(value)
	{
	case DI_LINEAR:
		m_FovInterp = new CLinearDoubleInterpolation<CamPathValue>(&m_FovView);
		break;
	default:
		m_FovInterp = new CCubicDoubleInterpolation<CamPathValue>(&m_FovView);
		break;
	}

	Changed();
}

CamPath::DoubleInterp CamPath::FovInterpMethod_get(void) const
{
	return m_FovInterpMethod;
}

void CamPath::Add(double time, const CamPathValue & value)
{
	ClearCurveData();
	ClearSequenceData();
	m_Map[time] = value;
	DoInterpolationMapChangedAll();
	Changed();
}

void CamPath::Changed()
{
	for(m_OnChangedIt = m_OnChanged.begin(); m_OnChangedIt != m_OnChanged.end(); m_OnChangedIt++) {
		m_OnChangedIt->Notify();
		if(m_OnChangedIt == m_OnChanged.end()) break;
	}
}

void CamPath::Remove(double time)
{
	ClearCurveData();
	ClearSequenceData();
	m_Map.erase(time);
	DoInterpolationMapChangedAll();
	Changed();
}

void CamPath::Clear()
{
	ClearCurveData();
	ClearSequenceData();
	m_DofEnabled = false;
	bool selectAll = true;

	CInterpolationMap<CamPathValue>::iterator last = m_Map.end();
	for(CInterpolationMap<CamPathValue>::iterator it = m_Map.begin(); it != m_Map.end();)
	{
		CInterpolationMap<CamPathValue>::iterator itNext = it;
		++itNext;

		if(it->second.Selected)
		{
			selectAll = false;
			m_Map.erase(it);
		}

		it = itNext;
	}

	if(selectAll) m_Map.clear();

	m_Offset = 0;

	DoInterpolationMapChangedAll();
	Changed();
}

size_t CamPath::GetSize() const
{
	return m_Map.size();
}

CamPathIterator CamPath::GetBegin()
{
	return CamPathIterator(m_Map.begin());
}

CamPathIterator CamPath::GetEnd()
{
	return CamPathIterator(m_Map.end());
}

double CamPath::GetLowerBound() const
{
	if(HasSequenceData()) {
		double result = DBL_MAX;
		for(CameraCut const & cut : m_CameraCuts) if(SequenceCameraAt(cut.Start)) result = std::min(result, cut.Start);
		return DBL_MAX == result ? 0.0 : result;
	}
	if(HasCurveData()) {
		double result = DBL_MAX;
		for(auto const & pair : m_CurveChannels) if(0 != pair.first.find("dof.") && !pair.second.Keys.empty()) result = std::min(result, pair.second.Keys.front().Time);
		if(DBL_MAX != result) return result;
	}
	if (m_Map.empty()) return 0.0;
	return m_Map.cbegin()->first;
}

double CamPath::GetUpperBound() const
{
	if(HasSequenceData()) {
		double result = -DBL_MAX;
		for(CameraCut const & cut : m_CameraCuts) {
			auto camera = m_SequenceCameras.find(cut.CameraId);
			if(m_SequenceCameras.end() != camera && camera->second->CanEval()) result = std::max(result, cut.End);
		}
		return -DBL_MAX == result ? 0.0 : result;
	}
	if(HasCurveData()) {
		double result = -DBL_MAX;
		for(auto const & pair : m_CurveChannels) if(0 != pair.first.find("dof.") && !pair.second.Keys.empty()) result = std::max(result, pair.second.Keys.back().Time);
		if(-DBL_MAX != result) return result;
	}
	if (m_Map.empty()) return 0.0;
	return m_Map.crbegin()->first;
}

bool CamPath::CanEval(void) const
{
	if(HasSequenceData()) {
		for(CameraCut const & cut : m_CameraCuts) {
			auto camera = m_SequenceCameras.find(cut.CameraId);
			if(m_SequenceCameras.end() != camera && camera->second->CanEval()) return true;
		}
		return false;
	}
	if(HasCurveData()) return CurveCanEval();
	return
		m_XInterp->CanEval()
		&& m_YInterp->CanEval()
		&& m_ZInterp->CanEval()
		&& m_RInterp->CanEval()
		&& m_FovInterp->CanEval()
		&& m_SelectedInterp->CanEval();
}

bool CamPath::CanEvalAt(double t) const
{
	if(!HasSequenceData()) return CanEval();
	CamPath * camera = SequenceCameraAt(t);
	return camera && camera->CanEvalAt(t);
}

CamPathValue CamPath::Eval(double t)
{
	if(HasSequenceData()) {
		CamPath * camera = SequenceCameraAt(t);
		return camera ? camera->Eval(t) : CamPathValue();
	}
	CamPathValue val;
	if(HasCurveData())
	{
		val.X = CurveValue("position.x", t);
		val.Y = CurveValue("position.y", t);
		val.Z = CurveValue("position.z", t);
		double pitch = CurveValue("rotation.pitch", t);
		double yaw = CurveValue("rotation.yaw", t);
		double roll = CurveValue("rotation.roll", t);
		val.R = Quaternion::FromQREulerAngles(QREulerAngles::FromQEulerAngles(QEulerAngles(pitch, yaw, roll)));
		val.Fov = CurveValue("fov", t, 90.0);
		val.Selected = false;
		val.HasDof = m_DofEnabled || m_CurveChannels.end() != m_CurveChannels.find("dof.nearBlurry");
		val.DofEnabled = m_DofEnabled;
		val.DofNearBlurry = CurveValue("dof.nearBlurry", t, -100.0);
		val.DofNearCrisp = CurveValue("dof.nearCrisp", t, 0.0);
		val.DofFarCrisp = CurveValue("dof.farCrisp", t, 180.0);
		val.DofFarBlurry = CurveValue("dof.farBlurry", t, 2000.0);
		val.DofMaxBlurSize = std::max(0.0, std::min(11.0, CurveValue("dof.maxBlur", t, 5.0)));
		val.DofRadiusScale = std::max(0.25, std::min(5.0, CurveValue("dof.radiusScale", t, 0.25)));
		return val;
	}

	val.X = m_XInterp->Eval(t);
	val.Y = m_YInterp->Eval(t);
	val.Z = m_ZInterp->Eval(t);
	val.R = m_RInterp->Eval(t);
	val.Fov = m_FovInterp->Eval(t);
	val.Selected = m_SelectedInterp->Eval(t);
	if(m_DofEnabled && !m_Map.empty())
	{
		auto upper = m_Map.lower_bound(t);
		auto lower = upper;
		if(m_Map.end() == upper) lower = --m_Map.end();
		else if(m_Map.begin() != upper && upper->first != t) --lower;
		auto const & a = lower->second;
		auto const & b = m_Map.end() == upper ? a : upper->second;
		double span = m_Map.end() == upper ? 0.0 : upper->first - lower->first;
		double u = 0.0 < span ? std::max(0.0, std::min(1.0, (t - lower->first) / span)) : 0.0;
		auto lerp = [u](double x, double y) { return x + (y - x) * u; };
		val.HasDof = true;
		val.DofEnabled = true;
		val.DofNearBlurry = lerp(a.DofNearBlurry, b.DofNearBlurry);
		val.DofNearCrisp = lerp(a.DofNearCrisp, b.DofNearCrisp);
		val.DofFarCrisp = lerp(a.DofFarCrisp, b.DofFarCrisp);
		val.DofFarBlurry = lerp(a.DofFarBlurry, b.DofFarBlurry);
		val.DofMaxBlurSize = std::max(0.0, std::min(11.0, lerp(a.DofMaxBlurSize, b.DofMaxBlurSize)));
		val.DofRadiusScale = std::max(0.25, std::min(5.0, lerp(a.DofRadiusScale, b.DofRadiusScale)));
	}

	return val;
}

bool CamPath::HasCurveData() const
{
	return !m_CurveChannels.empty();
}

bool CamPath::HasSequenceData() const
{
	return !m_SequenceCameras.empty();
}

bool CamPath::CurveCanEval() const
{
	static char const * required[] = { "position.x", "position.y", "position.z", "rotation.pitch", "rotation.yaw", "rotation.roll", "fov" };
	for(char const * id : required) {
		auto it = m_CurveChannels.find(id);
		if(m_CurveChannels.end() == it || it->second.Keys.empty()) return false;
	}
	return true;
}

double CamPath::CurveValue(char const * id, double time, double fallback) const
{
	auto it = m_CurveChannels.find(id);
	return m_CurveChannels.end() != it && !it->second.Keys.empty() ? it->second.Eval(time) : fallback;
}

void CamPath::ClearCurveData()
{
	m_CurveChannels.clear();
}

void CamPath::ClearSequenceData()
{
	m_SequenceCameras.clear();
	m_SequenceCameraNames.clear();
	m_CameraCuts.clear();
}

CamPath * CamPath::SequenceCameraAt(double time) const
{
	CamPath * result = nullptr;
	double bestStart = -DBL_MAX;
	for(CameraCut const & cut : m_CameraCuts) {
		if(cut.Start <= time && time < cut.End && bestStart <= cut.Start) {
			auto camera = m_SequenceCameras.find(cut.CameraId);
			if(m_SequenceCameras.end() != camera) {
				result = camera->second.get();
				bestStart = cut.Start;
			}
		}
	}
	return result;
}

void CamPath::RebuildSequenceSummaryMap()
{
	m_Map.clear();
	for(CameraCut const & cut : m_CameraCuts) {
		auto cameraIt = m_SequenceCameras.find(cut.CameraId);
		if(m_SequenceCameras.end() == cameraIt || !cameraIt->second->CanEval()) continue;
		CamPath * camera = cameraIt->second.get();
		m_Map[cut.Start] = camera->Eval(cut.Start);
		for(auto const & point : camera->m_Map) {
			if(cut.Start < point.first && point.first < cut.End)
				m_Map[point.first] = camera->Eval(point.first);
		}
	}
	DoInterpolationMapChangedAll();
}

void CamPath::RebuildCurveSummaryMap()
{
	std::set<double> times;
	for(auto const & pair : m_CurveChannels) {
		if(0 == pair.first.find("dof.")) continue;
		for(CurveKey const & key : pair.second.Keys) times.insert(key.Time);
	}
	m_Map.clear();
	for(double time : times) m_Map[time] = Eval(time);
	DoInterpolationMapChangedAll();
}

char * double2xml(rapidxml::xml_document<> & doc, double value)
{
	char szTmp[196];
	_snprintf_s(szTmp, _TRUNCATE,"%.17g", value);
	return doc.allocate_string(szTmp);
}

void CamPath::AppendXmlCamPath(rapidxml::xml_document<char> & doc, rapidxml::xml_node<char> * cam) const
{
	bool curves = HasCurveData();
	cam->append_attribute(doc.allocate_attribute("model", curves ? "curves" : "classic"));
	cam->append_attribute(doc.allocate_attribute("dofEnabled", m_DofEnabled ? "true" : "false"));
	if(DI_DEFAULT != m_PositionInterpMethod)
		cam->append_attribute(doc.allocate_attribute("positionInterp", DoubleInterp_ToString(m_PositionInterpMethod)));
	if(QI_DEFAULT != m_RotationInterpMethod)
		cam->append_attribute(doc.allocate_attribute("rotationInterp", QuaternionInterp_ToString(m_RotationInterpMethod)));
	if(DI_DEFAULT != m_FovInterpMethod)
		cam->append_attribute(doc.allocate_attribute("fovInterp", DoubleInterp_ToString(m_FovInterpMethod)));
	if(m_Hold) cam->append_attribute(doc.allocate_attribute("hold"));

	if(!curves)
	{
		auto points = doc.allocate_node(rapidxml::node_element, "points");
		cam->append_node(points);
		for(auto const & pair : m_Map)
		{
			double time = pair.first;
			CamPathValue const & value = pair.second;
			QEulerAngles angles = value.R.ToQREulerAngles().ToQEulerAngles();
			auto point = doc.allocate_node(rapidxml::node_element, "p");
			point->append_attribute(doc.allocate_attribute("t", double2xml(doc, time)));
			point->append_attribute(doc.allocate_attribute("x", double2xml(doc, value.X)));
			point->append_attribute(doc.allocate_attribute("y", double2xml(doc, value.Y)));
			point->append_attribute(doc.allocate_attribute("z", double2xml(doc, value.Z)));
			point->append_attribute(doc.allocate_attribute("fov", double2xml(doc, value.Fov)));
			point->append_attribute(doc.allocate_attribute("rx", double2xml(doc, angles.Roll)));
			point->append_attribute(doc.allocate_attribute("ry", double2xml(doc, angles.Pitch)));
			point->append_attribute(doc.allocate_attribute("rz", double2xml(doc, angles.Yaw)));
			point->append_attribute(doc.allocate_attribute("qw", double2xml(doc, value.R.W)));
			point->append_attribute(doc.allocate_attribute("qx", double2xml(doc, value.R.X)));
			point->append_attribute(doc.allocate_attribute("qy", double2xml(doc, value.R.Y)));
			point->append_attribute(doc.allocate_attribute("qz", double2xml(doc, value.R.Z)));
			if(value.Selected) point->append_attribute(doc.allocate_attribute("selected"));
			if(value.HasDof || m_DofEnabled) {
				point->append_attribute(doc.allocate_attribute("dofNearBlurry", double2xml(doc, value.DofNearBlurry)));
				point->append_attribute(doc.allocate_attribute("dofNearCrisp", double2xml(doc, value.DofNearCrisp)));
				point->append_attribute(doc.allocate_attribute("dofFarCrisp", double2xml(doc, value.DofFarCrisp)));
				point->append_attribute(doc.allocate_attribute("dofFarBlurry", double2xml(doc, value.DofFarBlurry)));
				point->append_attribute(doc.allocate_attribute("dofMaxBlurSize", double2xml(doc, value.DofMaxBlurSize)));
				point->append_attribute(doc.allocate_attribute("dofRadiusScale", double2xml(doc, value.DofRadiusScale)));
			}
			points->append_node(point);
		}
		return;
	}

	auto curveEditor = doc.allocate_node(rapidxml::node_element, "curveEditor");
	curveEditor->append_attribute(doc.allocate_attribute("version", "1"));
	curveEditor->append_attribute(doc.allocate_attribute("dofEnabled", m_DofEnabled ? "true" : "false"));
	cam->append_node(curveEditor);
	for(auto const & pair : m_CurveChannels)
	{
		CurveChannel const & channel = pair.second;
		auto channelNode = doc.allocate_node(rapidxml::node_element, "channel");
		channelNode->append_attribute(doc.allocate_attribute("id", doc.allocate_string(channel.Id.c_str())));
		channelNode->append_attribute(doc.allocate_attribute("name", doc.allocate_string(channel.Name.c_str())));
		channelNode->append_attribute(doc.allocate_attribute("group", doc.allocate_string(channel.Group.c_str())));
		channelNode->append_attribute(doc.allocate_attribute("color", doc.allocate_string(channel.Color.c_str())));
		curveEditor->append_node(channelNode);
		for(CurveKey const & key : channel.Keys)
		{
			auto keyNode = doc.allocate_node(rapidxml::node_element, "key");
			keyNode->append_attribute(doc.allocate_attribute("t", double2xml(doc, key.Time)));
			keyNode->append_attribute(doc.allocate_attribute("v", double2xml(doc, key.Value)));
			keyNode->append_attribute(doc.allocate_attribute("in", double2xml(doc, key.InTangent)));
			keyNode->append_attribute(doc.allocate_attribute("out", double2xml(doc, key.OutTangent)));
			keyNode->append_attribute(doc.allocate_attribute("inWeight", double2xml(doc, key.InWeight)));
			keyNode->append_attribute(doc.allocate_attribute("outWeight", double2xml(doc, key.OutWeight)));
			keyNode->append_attribute(doc.allocate_attribute("weighted", key.Weighted ? "true" : "false"));
			keyNode->append_attribute(doc.allocate_attribute("interpolation",
				CI_CONSTANT == key.Interpolation ? "Constant" : CI_LINEAR == key.Interpolation ? "Linear" : "Bezier"));
			keyNode->append_attribute(doc.allocate_attribute("tangentMode",
				CT_SMOOTH == key.TangentMode ? "Smooth" : CT_BROKEN == key.TangentMode ? "Broken"
				: CT_LINEAR == key.TangentMode ? "Linear" : "Auto"));
			channelNode->append_node(keyNode);
		}
	}
}

bool CamPath::Save(wchar_t const * fileName)
{
	rapidxml::xml_document<> doc;

	rapidxml::xml_node<> * decl = doc.allocate_node(rapidxml::node_declaration);
	decl->append_attribute(doc.allocate_attribute("version", "1.0"));
	decl->append_attribute(doc.allocate_attribute("encoding", "utf-8"));
	doc.append_node(decl);

	if(HasSequenceData())
	{
		auto sequence = doc.allocate_node(rapidxml::node_element, "campathSequence");
		sequence->append_attribute(doc.allocate_attribute("version", "1"));
		if(m_Offset) sequence->append_attribute(doc.allocate_attribute("offset", double2xml(doc, m_Offset)));
		if(m_Hold) sequence->append_attribute(doc.allocate_attribute("hold"));
		doc.append_node(sequence);

		auto cameras = doc.allocate_node(rapidxml::node_element, "cameras");
		sequence->append_node(cameras);
		for(auto const & pair : m_SequenceCameras)
		{
			auto camera = doc.allocate_node(rapidxml::node_element, "camera");
			camera->append_attribute(doc.allocate_attribute("id", doc.allocate_string(pair.first.c_str())));
			auto name = m_SequenceCameraNames.find(pair.first);
			if(m_SequenceCameraNames.end() != name)
				camera->append_attribute(doc.allocate_attribute("name", doc.allocate_string(name->second.c_str())));
			cameras->append_node(camera);
			auto campath = doc.allocate_node(rapidxml::node_element, "campath");
			camera->append_node(campath);
			pair.second->AppendXmlCamPath(doc, campath);
		}

		auto cuts = doc.allocate_node(rapidxml::node_element, "cameraCuts");
		sequence->append_node(cuts);
		for(CameraCut const & cut : m_CameraCuts)
		{
			auto node = doc.allocate_node(rapidxml::node_element, "cut");
			node->append_attribute(doc.allocate_attribute("start", double2xml(doc, cut.Start)));
			node->append_attribute(doc.allocate_attribute("end", double2xml(doc, cut.End)));
			node->append_attribute(doc.allocate_attribute("camera", doc.allocate_string(cut.CameraId.c_str())));
			cuts->append_node(node);
		}

		std::ofstream ofs(fileName, std::ios_base::binary);
		bool ok = !ofs.fail();
		if(ok) ofs << doc;
		if(ofs.fail()) ok = false;
		ofs.close();
		return ok;
	}

	rapidxml::xml_node<> * cam = doc.allocate_node(rapidxml::node_element, "campath");
	bool hasCurveData = HasCurveData();
	cam->append_attribute(doc.allocate_attribute("model", hasCurveData ? "curves" : "classic"));
	cam->append_attribute(doc.allocate_attribute("dofEnabled", m_DofEnabled ? "true" : "false"));
	if(DI_DEFAULT != m_PositionInterpMethod)
		cam->append_attribute(doc.allocate_attribute("positionInterp", DoubleInterp_ToString(m_PositionInterpMethod)));
	if(QI_DEFAULT != m_RotationInterpMethod)
		cam->append_attribute(doc.allocate_attribute("rotationInterp", QuaternionInterp_ToString(m_RotationInterpMethod)));
	if(DI_DEFAULT != m_FovInterpMethod)
		cam->append_attribute(doc.allocate_attribute("fovInterp", DoubleInterp_ToString(m_FovInterpMethod)));
	if (m_Offset)
		cam->append_attribute(doc.allocate_attribute("offset", double2xml(doc, m_Offset)));
	if (m_Hold)
		cam->append_attribute(doc.allocate_attribute("hold"));
	doc.append_node(cam);

	if(!hasCurveData)
	{
		rapidxml::xml_node<> * pts = doc.allocate_node(rapidxml::node_element, "points");
		cam->append_node(pts);

		rapidxml::xml_node<> * cmt = doc.allocate_node(rapidxml::node_comment,0,
			"Points are in Quake coordinates, meaning x=forward, y=left, z=up and rotation order is first rx, then ry and lastly rz.\n"
			"Rotation direction follows the right-hand grip rule.\n"
			"rx (roll), ry (pitch), rz(yaw) are the Euler angles in degrees.\n"
			"qw, qx, qy, qz are the quaternion values.\n"
			"When read it is sufficient that either rx, ry, rz OR qw, qx, qy, qz are present.\n"
			"If both are present then qw, qx, qy, qz take precedence."
		);
		pts->append_node(cmt);

		for(CamPathIterator it = GetBegin(); it != GetEnd(); ++it)
		{
			double time = it.GetTime();
			CamPathValue val = it.GetValue();
			QEulerAngles ang = val.R.ToQREulerAngles().ToQEulerAngles();

			rapidxml::xml_node<> * pt = doc.allocate_node(rapidxml::node_element, "p");
			pt->append_attribute(doc.allocate_attribute("t", double2xml(doc,time)));
			pt->append_attribute(doc.allocate_attribute("x", double2xml(doc,val.X)));
			pt->append_attribute(doc.allocate_attribute("y", double2xml(doc,val.Y)));
			pt->append_attribute(doc.allocate_attribute("z", double2xml(doc,val.Z)));
			pt->append_attribute(doc.allocate_attribute("fov", double2xml(doc,val.Fov)));
			pt->append_attribute(doc.allocate_attribute("rx", double2xml(doc,ang.Roll)));
			pt->append_attribute(doc.allocate_attribute("ry", double2xml(doc,ang.Pitch)));
			pt->append_attribute(doc.allocate_attribute("rz", double2xml(doc,ang.Yaw)));
			pt->append_attribute(doc.allocate_attribute("qw", double2xml(doc,it.wrapped->second.R.W)));
			pt->append_attribute(doc.allocate_attribute("qx", double2xml(doc,it.wrapped->second.R.X)));
			pt->append_attribute(doc.allocate_attribute("qy", double2xml(doc,it.wrapped->second.R.Y)));
			pt->append_attribute(doc.allocate_attribute("qz", double2xml(doc,it.wrapped->second.R.Z)));

			if(val.Selected)
				pt->append_attribute(doc.allocate_attribute("selected"));
			if(val.HasDof) {
				if(val.DofEnabled) pt->append_attribute(doc.allocate_attribute("dofEnabled"));
				pt->append_attribute(doc.allocate_attribute("dofNearBlurry", double2xml(doc, val.DofNearBlurry)));
				pt->append_attribute(doc.allocate_attribute("dofNearCrisp", double2xml(doc, val.DofNearCrisp)));
				pt->append_attribute(doc.allocate_attribute("dofFarCrisp", double2xml(doc, val.DofFarCrisp)));
				pt->append_attribute(doc.allocate_attribute("dofFarBlurry", double2xml(doc, val.DofFarBlurry)));
				pt->append_attribute(doc.allocate_attribute("dofMaxBlurSize", double2xml(doc, val.DofMaxBlurSize)));
				pt->append_attribute(doc.allocate_attribute("dofRadiusScale", double2xml(doc, val.DofRadiusScale)));
			}

			pts->append_node(pt);
		}
	}

	if(hasCurveData)
	{
		rapidxml::xml_node<> * curveEditor = doc.allocate_node(rapidxml::node_element, "curveEditor");
		curveEditor->append_attribute(doc.allocate_attribute("version", "1"));
		curveEditor->append_attribute(doc.allocate_attribute("dofEnabled", m_DofEnabled ? "true" : "false"));
		cam->append_node(curveEditor);
		for(auto const & pair : m_CurveChannels)
		{
			CurveChannel const & channel = pair.second;
			rapidxml::xml_node<> * channelNode = doc.allocate_node(rapidxml::node_element, "channel");
			channelNode->append_attribute(doc.allocate_attribute("id", doc.allocate_string(channel.Id.c_str())));
			channelNode->append_attribute(doc.allocate_attribute("name", doc.allocate_string(channel.Name.c_str())));
			channelNode->append_attribute(doc.allocate_attribute("group", doc.allocate_string(channel.Group.c_str())));
			channelNode->append_attribute(doc.allocate_attribute("color", doc.allocate_string(channel.Color.c_str())));
			curveEditor->append_node(channelNode);
			for(CurveKey const & key : channel.Keys)
			{
				rapidxml::xml_node<> * keyNode = doc.allocate_node(rapidxml::node_element, "key");
				keyNode->append_attribute(doc.allocate_attribute("t", double2xml(doc, key.Time)));
				keyNode->append_attribute(doc.allocate_attribute("v", double2xml(doc, key.Value)));
				keyNode->append_attribute(doc.allocate_attribute("in", double2xml(doc, key.InTangent)));
				keyNode->append_attribute(doc.allocate_attribute("out", double2xml(doc, key.OutTangent)));
				keyNode->append_attribute(doc.allocate_attribute("inWeight", double2xml(doc, key.InWeight)));
				keyNode->append_attribute(doc.allocate_attribute("outWeight", double2xml(doc, key.OutWeight)));
				keyNode->append_attribute(doc.allocate_attribute("weighted", key.Weighted ? "true" : "false"));
				char const * interpolation = CI_CONSTANT == key.Interpolation ? "Constant" : CI_LINEAR == key.Interpolation ? "Linear" : "Bezier";
				char const * tangentMode = CT_SMOOTH == key.TangentMode ? "Smooth" : CT_BROKEN == key.TangentMode ? "Broken" : CT_LINEAR == key.TangentMode ? "Linear" : "Auto";
				keyNode->append_attribute(doc.allocate_attribute("interpolation", interpolation));
				keyNode->append_attribute(doc.allocate_attribute("tangentMode", tangentMode));
				channelNode->append_node(keyNode);
			}
		}
	}

	std::string xmlString;
	rapidxml::print(std::back_inserter(xmlString), doc);

	std::ofstream ofs(fileName, std::ios_base::binary);

	bool bOk = !ofs.fail();

	if (bOk)
	{
		ofs << doc;
	}

	if (ofs.fail())
		bOk = false;

	ofs.close();
	
	return bOk;
}

bool CamPath::LoadXmlCamPath(rapidxml::xml_node<char> * camNode)
{
	if(!camNode) return false;

	m_Map.clear();
	ClearCurveData();
	ClearSequenceData();

	rapidxml::xml_attribute<> * modelA = camNode->first_attribute("model");
	bool curveModel = modelA && 0 == _stricmp(modelA->value(), "curves");
	rapidxml::xml_attribute<> * dofEnabledA = camNode->first_attribute("dofEnabled");
	m_DofEnabled = dofEnabledA && 0 != _stricmp(dofEnabledA->value(), "false");

	DoubleInterp positionInterp = DI_DEFAULT;
	if(auto attr = camNode->first_attribute("positionInterp")) DoubleInterp_FromString(attr->value(), positionInterp);
	PositionInterpMethod_set(positionInterp);
	QuaternionInterp rotationInterp = QI_DEFAULT;
	if(auto attr = camNode->first_attribute("rotationInterp")) QuaternionInterp_FromString(attr->value(), rotationInterp);
	RotationInterpMethod_set(rotationInterp);
	DoubleInterp fovInterp = DI_DEFAULT;
	if(auto attr = camNode->first_attribute("fovInterp")) DoubleInterp_FromString(attr->value(), fovInterp);
	FovInterpMethod_set(fovInterp);

	SetOffset(camNode->first_attribute("offset") ? atof(camNode->first_attribute("offset")->value()) : 0.0);
	SetHold(nullptr != camNode->first_attribute("hold"));

	if(!curveModel)
	{
		rapidxml::xml_node<> * pointsNode = camNode->first_node("points");
		for(rapidxml::xml_node<> * point = pointsNode ? pointsNode->first_node("p") : nullptr;
			point; point = point->next_sibling("p"))
		{
			auto timeA = point->first_attribute("t");
			if(!timeA) continue;
			CamPathValue value;
			if(auto attr = point->first_attribute("x")) value.X = atof(attr->value());
			if(auto attr = point->first_attribute("y")) value.Y = atof(attr->value());
			if(auto attr = point->first_attribute("z")) value.Z = atof(attr->value());
			if(auto attr = point->first_attribute("fov")) value.Fov = atof(attr->value());
			auto qwA = point->first_attribute("qw");
			auto qxA = point->first_attribute("qx");
			auto qyA = point->first_attribute("qy");
			auto qzA = point->first_attribute("qz");
			if(qwA && qxA && qyA && qzA) {
				value.R.W = atof(qwA->value()); value.R.X = atof(qxA->value());
				value.R.Y = atof(qyA->value()); value.R.Z = atof(qzA->value());
			}
			else {
				double roll = point->first_attribute("rx") ? atof(point->first_attribute("rx")->value()) : 0.0;
				double pitch = point->first_attribute("ry") ? atof(point->first_attribute("ry")->value()) : 0.0;
				double yaw = point->first_attribute("rz") ? atof(point->first_attribute("rz")->value()) : 0.0;
				value.R = Quaternion::FromQREulerAngles(
					QREulerAngles::FromQEulerAngles(QEulerAngles(pitch, yaw, roll)));
			}
			value.Selected = nullptr != point->first_attribute("selected");
			value.HasDof = m_DofEnabled;
			value.DofEnabled = m_DofEnabled;
			if(auto attr = point->first_attribute("dofNearBlurry")) value.DofNearBlurry = atof(attr->value());
			if(auto attr = point->first_attribute("dofNearCrisp")) value.DofNearCrisp = atof(attr->value());
			if(auto attr = point->first_attribute("dofFarCrisp")) value.DofFarCrisp = atof(attr->value());
			if(auto attr = point->first_attribute("dofFarBlurry")) value.DofFarBlurry = atof(attr->value());
			if(auto attr = point->first_attribute("dofMaxBlurSize"))
				value.DofMaxBlurSize = std::max(0.0, std::min(11.0, atof(attr->value())));
			if(auto attr = point->first_attribute("dofRadiusScale"))
				value.DofRadiusScale = std::max(0.25, std::min(5.0, atof(attr->value())));
			m_Map[atof(timeA->value())] = value;
		}
	}
	else if(rapidxml::xml_node<> * curveEditor = camNode->first_node("curveEditor"))
	{
		if(auto attr = curveEditor->first_attribute("dofEnabled"))
			m_DofEnabled = 0 != _stricmp(attr->value(), "false");
		for(rapidxml::xml_node<> * channelNode = curveEditor->first_node("channel"); channelNode;
			channelNode = channelNode->next_sibling("channel"))
		{
			auto idA = channelNode->first_attribute("id");
			if(!idA || !*idA->value()) continue;
			CurveChannel channel;
			channel.Id = idA->value();
			channel.Name = channelNode->first_attribute("name") ? channelNode->first_attribute("name")->value() : channel.Id;
			channel.Group = channelNode->first_attribute("group") ? channelNode->first_attribute("group")->value() : "Other";
			channel.Color = channelNode->first_attribute("color") ? channelNode->first_attribute("color")->value() : "#FFFFFF";
			for(rapidxml::xml_node<> * keyNode = channelNode->first_node("key"); keyNode;
				keyNode = keyNode->next_sibling("key"))
			{
				auto timeA = keyNode->first_attribute("t");
				auto valueA = keyNode->first_attribute("v");
				if(!timeA || !valueA) continue;
				CurveKey key;
				key.Time = atof(timeA->value()); key.Value = atof(valueA->value());
				if(auto attr = keyNode->first_attribute("in")) key.InTangent = atof(attr->value());
				if(auto attr = keyNode->first_attribute("out")) key.OutTangent = atof(attr->value());
				if(auto attr = keyNode->first_attribute("inWeight")) key.InWeight = atof(attr->value());
				if(auto attr = keyNode->first_attribute("outWeight")) key.OutWeight = atof(attr->value());
				if(auto attr = keyNode->first_attribute("weighted")) key.Weighted = 0 == _stricmp(attr->value(), "true");
				if(auto attr = keyNode->first_attribute("interpolation")) {
					if(0 == _stricmp(attr->value(), "Constant")) key.Interpolation = CI_CONSTANT;
					else if(0 == _stricmp(attr->value(), "Linear")) key.Interpolation = CI_LINEAR;
				}
				if(auto attr = keyNode->first_attribute("tangentMode")) {
					if(0 == _stricmp(attr->value(), "Smooth")) key.TangentMode = CT_SMOOTH;
					else if(0 == _stricmp(attr->value(), "Broken")) key.TangentMode = CT_BROKEN;
					else if(0 == _stricmp(attr->value(), "Linear")) key.TangentMode = CT_LINEAR;
				}
				channel.Keys.push_back(key);
			}
			std::sort(channel.Keys.begin(), channel.Keys.end(),
				[](CurveKey const & a, CurveKey const & b) { return a.Time < b.Time; });
			m_CurveChannels[channel.Id] = channel;
		}
		if(CurveCanEval()) RebuildCurveSummaryMap(); else ClearCurveData();
	}

	DoInterpolationMapChangedAll();
	return CanEval();
}

bool CamPath::Load(wchar_t const * fileName)
{
	bool bOk = false;

	FILE * pFile = 0;

	_wfopen_s(&pFile, fileName, L"rb");

	if(!pFile)
		return false;
	
	fseek(pFile, 0, SEEK_END);
	size_t fileSize = ftell(pFile);
	rewind(pFile);

	char * pData = new char[fileSize+1];
	pData[fileSize] = 0;

	size_t readSize = fread(pData, sizeof(char), fileSize, pFile);
	bOk = readSize == fileSize;
	if(bOk)
	{
		try
		{
			do
			{
				rapidxml::xml_document<> doc;
				doc.parse<0>(pData);

				if(rapidxml::xml_node<> * campathNode = doc.first_node("campath"))
				{
					LoadXmlCamPath(campathNode);
					break;
				}

				if(rapidxml::xml_node<> * sequenceNode = doc.first_node("campathSequence"))
				{
					m_Map.clear();
					ClearCurveData();
					ClearSequenceData();
					m_DofEnabled = false;
					SetOffset(sequenceNode->first_attribute("offset")
						? atof(sequenceNode->first_attribute("offset")->value()) : 0.0);
					SetHold(nullptr != sequenceNode->first_attribute("hold"));

					rapidxml::xml_node<> * camerasNode = sequenceNode->first_node("cameras");
					for(rapidxml::xml_node<> * cameraNode = camerasNode ? camerasNode->first_node("camera") : nullptr;
						cameraNode; cameraNode = cameraNode->next_sibling("camera"))
					{
						auto idA = cameraNode->first_attribute("id");
						auto campathNode = cameraNode->first_node("campath");
						if(!idA || !*idA->value() || !campathNode) continue;
						std::unique_ptr<CamPath> camera(new CamPath());
						camera->LoadXmlCamPath(campathNode);
						camera->SetOffset(0.0);
						m_SequenceCameras[idA->value()] = std::move(camera);
						auto nameA = cameraNode->first_attribute("name");
						m_SequenceCameraNames[idA->value()] = nameA ? nameA->value() : idA->value();
					}

					rapidxml::xml_node<> * cutsNode = sequenceNode->first_node("cameraCuts");
					for(rapidxml::xml_node<> * cutNode = cutsNode ? cutsNode->first_node("cut") : nullptr;
						cutNode; cutNode = cutNode->next_sibling("cut"))
					{
						auto startA = cutNode->first_attribute("start");
						auto endA = cutNode->first_attribute("end");
						auto cameraA = cutNode->first_attribute("camera");
						if(!startA || !endA || !cameraA) continue;
						CameraCut cut;
						cut.Start = atof(startA->value());
						cut.End = atof(endA->value());
						cut.CameraId = cameraA->value();
						if(cut.Start < cut.End) m_CameraCuts.push_back(cut);
					}
					std::sort(m_CameraCuts.begin(), m_CameraCuts.end(),
						[](CameraCut const & a, CameraCut const & b) { return a.Start < b.Start; });
					RebuildSequenceSummaryMap();
					break;
				}

				rapidxml::xml_node<> * cur_node = doc.first_node("campath");
				if(!cur_node) break;
				rapidxml::xml_node<> * camNode = cur_node;
				rapidxml::xml_attribute<> * modelA = camNode->first_attribute("model");
				bool curveModel = modelA && 0 == _stricmp(modelA->value(), "curves");

				// Clear current Campath:
				SelectNone();
				Clear();

				rapidxml::xml_attribute<> * positionInterpA = cur_node->first_attribute("positionInterp");
				DoubleInterp positionInterp = DI_DEFAULT;
				if(positionInterpA) DoubleInterp_FromString(positionInterpA->value(), positionInterp);
				PositionInterpMethod_set(positionInterp);

				rapidxml::xml_attribute<> * rotationInterpA = cur_node->first_attribute("rotationInterp");
				QuaternionInterp rotationInterp = QI_DEFAULT;
				if(rotationInterpA) QuaternionInterp_FromString(rotationInterpA->value(), rotationInterp);
				RotationInterpMethod_set(rotationInterp);

				rapidxml::xml_attribute<> * fovInterpA = cur_node->first_attribute("fovInterp");
				DoubleInterp fovInterp = DI_DEFAULT;
				if(fovInterpA) DoubleInterp_FromString(fovInterpA->value(), fovInterp);
				FovInterpMethod_set(fovInterp);

				rapidxml::xml_attribute<>* offsetA = cur_node->first_attribute("offset");
				double offset = offsetA ? atof(offsetA->value()) : 0.0;
				SetOffset(offset);

				rapidxml::xml_attribute<> * holdA = cur_node->first_attribute("hold");
				bool bHold = nullptr != holdA;
				SetHold(bHold);

				rapidxml::xml_node<> * pointsNode = curveModel ? nullptr : camNode->first_node("points");

				for(cur_node = pointsNode ? pointsNode->first_node("p") : nullptr; cur_node; cur_node = cur_node->next_sibling("p"))
				{
					rapidxml::xml_attribute<> * timeAttr = cur_node->first_attribute("t");
					if(!timeAttr) continue;

					rapidxml::xml_attribute<> * xA = cur_node->first_attribute("x");
					rapidxml::xml_attribute<> * yA = cur_node->first_attribute("y");
					rapidxml::xml_attribute<> * zA = cur_node->first_attribute("z");
					rapidxml::xml_attribute<> * fovA = cur_node->first_attribute("fov");
					rapidxml::xml_attribute<> * rxA = cur_node->first_attribute("rx");
					rapidxml::xml_attribute<> * ryA = cur_node->first_attribute("ry");
					rapidxml::xml_attribute<> * rzA = cur_node->first_attribute("rz");
					rapidxml::xml_attribute<> * qwA = cur_node->first_attribute("qw");
					rapidxml::xml_attribute<> * qxA = cur_node->first_attribute("qx");
					rapidxml::xml_attribute<> * qyA = cur_node->first_attribute("qy");
					rapidxml::xml_attribute<> * qzA = cur_node->first_attribute("qz");
					rapidxml::xml_attribute<> * selectedA = cur_node->first_attribute("selected");

					double dT = atof(timeAttr->value());
					double dX = xA ? atof(xA->value()) : 0.0;
					double dY = yA ? atof(yA->value()) : 0.0;
					double dZ = zA ? atof(zA->value()) : 0.0;
					double dFov = fovA ? atof(fovA->value()) : 90.0;

					if(qwA && qxA && qyA && qzA)
					{
						CamPathValue r;
						r.X = dX;
						r.Y = dY;
						r.Z = dZ;
						r.R.W = atof(qwA->value());
						r.R.X = atof(qxA->value());
						r.R.Y = atof(qyA->value());
						r.R.Z = atof(qzA->value());
						r.Fov = dFov;
						r.Selected = 0 != selectedA;

						// Add point:
						m_Map[dT] = r;
					}
					else
					{
						double dRXroll = rxA ? atof(rxA->value()) : 0.0;
						double dRYpitch = ryA ? atof(ryA->value()) : 0.0;
						double dRZyaw = rzA ? atof(rzA->value()) : 0.0;

						CamPathValue r;
						r.X = dX;
						r.Y = dY;
						r.Z = dZ;
						r.R = Quaternion::FromQREulerAngles(QREulerAngles::FromQEulerAngles(QEulerAngles(dRYpitch, dRZyaw, dRXroll)));
						r.Fov = dFov;
						r.Selected = 0 != selectedA;

						// Add point:
						m_Map[dT] = r;
					}
				}

				rapidxml::xml_node<> * curveEditorNode = curveModel ? camNode->first_node("curveEditor") : nullptr;
				if(curveEditorNode)
				{
					for(rapidxml::xml_node<> * channelNode = curveEditorNode->first_node("channel"); channelNode; channelNode = channelNode->next_sibling("channel"))
					{
						rapidxml::xml_attribute<> * idA = channelNode->first_attribute("id");
						if(!idA || !*idA->value()) continue;
						CurveChannel channel;
						channel.Id = idA->value();
						rapidxml::xml_attribute<> * nameA = channelNode->first_attribute("name");
						rapidxml::xml_attribute<> * groupA = channelNode->first_attribute("group");
						rapidxml::xml_attribute<> * colorA = channelNode->first_attribute("color");
						channel.Name = nameA ? nameA->value() : channel.Id;
						channel.Group = groupA ? groupA->value() : "Other";
						channel.Color = colorA ? colorA->value() : "#FFFFFF";
						for(rapidxml::xml_node<> * keyNode = channelNode->first_node("key"); keyNode; keyNode = keyNode->next_sibling("key"))
						{
							rapidxml::xml_attribute<> * timeA = keyNode->first_attribute("t");
							rapidxml::xml_attribute<> * valueA = keyNode->first_attribute("v");
							if(!timeA || !valueA) continue;
							CurveKey key;
							key.Time = atof(timeA->value());
							key.Value = atof(valueA->value());
							rapidxml::xml_attribute<> * inA = keyNode->first_attribute("in");
							rapidxml::xml_attribute<> * outA = keyNode->first_attribute("out");
							rapidxml::xml_attribute<> * inWeightA = keyNode->first_attribute("inWeight");
							rapidxml::xml_attribute<> * outWeightA = keyNode->first_attribute("outWeight");
							rapidxml::xml_attribute<> * weightedA = keyNode->first_attribute("weighted");
							rapidxml::xml_attribute<> * interpolationA = keyNode->first_attribute("interpolation");
							rapidxml::xml_attribute<> * tangentModeA = keyNode->first_attribute("tangentMode");
							key.InTangent = inA ? atof(inA->value()) : 0.0;
							key.OutTangent = outA ? atof(outA->value()) : 0.0;
							key.InWeight = inWeightA ? atof(inWeightA->value()) : 0.25;
							key.OutWeight = outWeightA ? atof(outWeightA->value()) : 0.25;
							key.Weighted = weightedA && 0 == _stricmp(weightedA->value(), "true");
							if(interpolationA && 0 == _stricmp(interpolationA->value(), "Constant")) key.Interpolation = CI_CONSTANT;
							else if(interpolationA && 0 == _stricmp(interpolationA->value(), "Linear")) key.Interpolation = CI_LINEAR;
							else key.Interpolation = CI_BEZIER;
							if(tangentModeA && 0 == _stricmp(tangentModeA->value(), "Smooth")) key.TangentMode = CT_SMOOTH;
							else if(tangentModeA && 0 == _stricmp(tangentModeA->value(), "Broken")) key.TangentMode = CT_BROKEN;
							else if(tangentModeA && 0 == _stricmp(tangentModeA->value(), "Linear")) key.TangentMode = CT_LINEAR;
							else key.TangentMode = CT_AUTO;
							channel.Keys.push_back(key);
						}
						std::sort(channel.Keys.begin(), channel.Keys.end(), [](CurveKey const & a, CurveKey const & b) { return a.Time < b.Time; });
						m_CurveChannels[channel.Id] = channel;
					}
					if(CurveCanEval()) RebuildCurveSummaryMap();
					else ClearCurveData();
				}
			}
			while (false);
		}
		catch(rapidxml::parse_error &)
		{
			bOk=false;
		}
	}

	delete pData;

	fclose(pFile);

	DoInterpolationMapChangedAll();
	Changed();

	return bOk;
}

size_t CamPath::SelectAll()
{
	for(CInterpolationMap<CamPathValue>::iterator it = m_Map.begin(); it != m_Map.end(); ++it)
	{
		it->second.Selected = true;
	}

	m_SelectedInterp->InterpolationMapChanged();
	Changed();

	return m_Map.size();
}

void CamPath::SelectNone()
{
	for(CInterpolationMap<CamPathValue>::iterator it = m_Map.begin(); it != m_Map.end(); ++it)
	{
		it->second.Selected = false;
	}

	m_SelectedInterp->InterpolationMapChanged();
	Changed();
}

size_t CamPath::SelectInvert()
{
	size_t selected = 0;

	for(CInterpolationMap<CamPathValue>::iterator it = m_Map.begin(); it != m_Map.end(); ++it)
	{
		it->second.Selected = !it->second.Selected;

		if(it->second.Selected) ++selected;
	}

	m_SelectedInterp->InterpolationMapChanged();
	Changed();

	return selected;
}

size_t CamPath::SelectAdd(size_t min, size_t max)
{
	size_t i = 0;
	size_t selected = 0;

	for(CInterpolationMap<CamPathValue>::iterator it = m_Map.begin(); it != m_Map.end(); ++it)
	{
		it->second.Selected = it->second.Selected || min <= i && i <= max;

		if(it->second.Selected) ++selected;

		++i;
	}

	m_SelectedInterp->InterpolationMapChanged();
	Changed();

	return selected;
}

size_t CamPath::SelectAdd(double min, size_t count)
{
	size_t selected = 0;

	for(CInterpolationMap<CamPathValue>::iterator it = m_Map.begin(); it != m_Map.end(); ++it)
	{
		it->second.Selected = it->second.Selected || min <= it->first && selected < count;

		if(it->second.Selected) ++selected;
	}

	m_SelectedInterp->InterpolationMapChanged();
	Changed();

	return selected;
}

size_t CamPath::SelectAdd(double min, double max)
{
	size_t selected = 0;

	for(CInterpolationMap<CamPathValue>::iterator it = m_Map.begin(); it != m_Map.end(); ++it)
	{
		it->second.Selected = it->second.Selected || min <= it->first && it->first <= max;

		if(it->second.Selected) ++selected;
	}

	m_SelectedInterp->InterpolationMapChanged();
	Changed();

	return selected;
}

void CamPath::SetStart(double t, bool relative)
{
	if(HasCurveData()) {
		double deltaT = relative ? t : t - GetLowerBound();
		for(auto & pair : m_CurveChannels) for(CurveKey & key : pair.second.Keys) key.Time += deltaT;
		RebuildCurveSummaryMap();
		Changed();
		return;
	}
	if(m_Map.size()<1) return;

	CInterpolationMap<CamPathValue> tempMap;

	bool selectAll = true;
	double first = 0;

	for(CInterpolationMap<CamPathValue>::iterator it = m_Map.begin(); it != m_Map.end(); ++it)
	{
		if(it->second.Selected)
		{
			if(selectAll)
			{
				selectAll = false;
				first = it->first;
				break;
			}
		}
	}

	double deltaT = relative ? t : (selectAll ? t -m_Map.begin()->first : t -first);

	for(CInterpolationMap<CamPathValue>::iterator it = m_Map.begin(); it != m_Map.end(); ++it)
	{
		double curT = it->first;
		CamPathValue curValue = it->second;

		if(selectAll || curValue.Selected)
		{
			tempMap[deltaT+curT] = curValue;
		}
		else
		{
			tempMap[curT] = curValue;
		}
	}

	CopyMap(m_Map, tempMap);

	DoInterpolationMapChangedAll();

	Changed();
}
	
void CamPath::SetDuration(double t)
{
	if(HasCurveData()) {
		double first = GetLowerBound();
		double duration = GetDuration();
		if(duration <= 0.0) return;
		double scale = t / duration;
		for(auto & pair : m_CurveChannels) for(CurveKey & key : pair.second.Keys) {
			key.Time = first + scale * (key.Time - first);
			key.InWeight *= fabs(scale);
			key.OutWeight *= fabs(scale);
			if(0.0 != scale) { key.InTangent /= scale; key.OutTangent /= scale; }
		}
		RebuildCurveSummaryMap();
		Changed();
		return;
	}
	if(m_Map.size()<2) return;

	CInterpolationMap<CamPathValue> tempMap;

	CopyMap(tempMap, m_Map);

	bool selectAll = true;
	double first = 0, last = 0;

	for(CInterpolationMap<CamPathValue>::iterator it = m_Map.begin(); it != m_Map.end(); ++it)
	{
		if(it->second.Selected)
		{
			if(selectAll)
			{
				selectAll = false;
				first = it->first;
				last = first;
			}
			else
			{
				last = it->first;
			}
		}
	}

	double oldDuration = selectAll ? GetDuration() : last -first;

	m_Map.clear();

	double scale = oldDuration ? t / oldDuration : 0.0;
	bool isFirst = true;
	double firstT = 0;

	for(CInterpolationMap<CamPathValue>::const_iterator it = tempMap.begin(); it != tempMap.end(); ++it)
	{
		double curT = it->first;
		CamPathValue curValue = it->second;

		if(selectAll || curValue.Selected)
		{
			if(isFirst)
			{
				m_Map[curT] = curValue;
				firstT = curT;
				isFirst = false;
			}
			else
				m_Map[firstT+scale*(curT-firstT)] = curValue;
		}
		else
			m_Map[curT] = curValue;
	}

	DoInterpolationMapChangedAll();

	Changed();
}

void CamPath::SetPosition(double x, double y, double z, bool setX, bool setY, bool setZ)
{
	ClearCurveData();
	if(m_Map.size()<1) return;

	bool selectAll = true;

	for(CInterpolationMap<CamPathValue>::const_iterator it = m_Map.begin(); it != m_Map.end(); ++it)
	{
		if(it->second.Selected)
		{
			if(selectAll)
			{
				selectAll = false;
				break;
			}
		}
	}

	// calcualte mid:

	double minX = 0, maxX = 0, minY = 0, maxY = 0, minZ = 0, maxZ = 0;
	bool first = true;

	for(CInterpolationMap<CamPathValue>::const_iterator it = m_Map.begin(); it != m_Map.end(); ++it)
	{
		if(selectAll || it->second.Selected)
		{
			CamPathValue curValue = it->second;

			if(first)
			{
				minX = curValue.X;
				minY = curValue.Y;
				minZ = curValue.Z;
				maxX = curValue.X;
				maxY = curValue.Y;
				maxZ = curValue.Z;
				first = false;
			}
			else
			{
				minX = std::min(minX, curValue.X);
				minY = std::min(minY, curValue.Y);
				minZ = std::min(minZ, curValue.Z);
				maxX = std::max(maxX, curValue.X);
				maxY = std::max(maxY, curValue.Y);
				maxZ = std::max(maxZ, curValue.Z);
			}
		}
	}

	double x0 = (maxX +minX) / 2;
	double y0 = (maxY +minY) / 2;
	double z0 = (maxZ +minZ) / 2;

	for(CInterpolationMap<CamPathValue>::iterator it = m_Map.begin(); it != m_Map.end(); ++it)
	{
		double curT = it->first;
		CamPathValue curValue = it->second;

		if(selectAll || curValue.Selected)
		{
			if(setX) curValue.X = x +(curValue.X -x0);
			if(setY) curValue.Y = y +(curValue.Y -y0);
			if(setZ) curValue.Z = z +(curValue.Z -z0);

			it->second = curValue;
		}
	}

	m_XInterp->InterpolationMapChanged();
	m_YInterp->InterpolationMapChanged();
	m_ZInterp->InterpolationMapChanged();

	Changed();
}

void CamPath::SetAngles(double yPitch, double zYaw, double xRoll, bool setY, bool setZ, bool setX)
{
	ClearCurveData();
	if(m_Map.size()<1) return;

	bool selectAll = true;

	for(CInterpolationMap<CamPathValue>::const_iterator it = m_Map.begin(); it != m_Map.end(); ++it)
	{
		if(it->second.Selected)
		{
			if(selectAll)
			{
				selectAll = false;
				break;
			}
		}
	}

	for(CInterpolationMap<CamPathValue>::iterator it = m_Map.begin(); it != m_Map.end(); ++it)
	{
		double curT = it->first;
		CamPathValue curValue = it->second;

		if(selectAll || curValue.Selected)
		{
			if(setY && setZ && setX) {
				curValue.R = Quaternion::FromQREulerAngles(QREulerAngles::FromQEulerAngles(QEulerAngles(yPitch, zYaw, xRoll)));
			} else {
				QEulerAngles angles = curValue.R.ToQREulerAngles().ToQEulerAngles();
				curValue.R = Quaternion::FromQREulerAngles(QREulerAngles::FromQEulerAngles(QEulerAngles(
					(setY ? yPitch : angles.Pitch),
					(setZ ? zYaw : angles.Yaw),
					(setX ? xRoll : angles.Roll)
				)));
			}

			it->second = curValue;
		}

	}

	m_RInterp->InterpolationMapChanged();

	Changed();
}

void CamPath::SetFov(double fov)
{
	ClearCurveData();
	if(m_Map.size()<1) return;

	bool selectAll = true;

	for(CInterpolationMap<CamPathValue>::const_iterator it = m_Map.begin(); it != m_Map.end(); ++it)
	{
		if(it->second.Selected)
		{
			if(selectAll)
			{
				selectAll = false;
				break;
			}
		}
	}

	for(CInterpolationMap<CamPathValue>::iterator it = m_Map.begin(); it != m_Map.end(); ++it)
	{
		double curT = it->first;
		CamPathValue curValue = it->second;

		if(selectAll || curValue.Selected)
		{
			curValue.Fov = fov;

			it->second = curValue;
		}

	}

	m_FovInterp->InterpolationMapChanged();

	Changed();
}

void CamPath::Rotate(double yPitch, double zYaw, double xRoll)
{
	ClearCurveData();
	if(m_Map.size()<1) return;

	bool selectAll = true;

	for(CInterpolationMap<CamPathValue>::const_iterator it = m_Map.begin(); it != m_Map.end(); ++it)
	{
		if(it->second.Selected)
		{
			if(selectAll)
			{
				selectAll = false;
				break;
			}
		}
	}

	// calcualte mid:

	double minX = 0, maxX = 0, minY = 0, maxY = 0, minZ = 0, maxZ = 0;
	bool first = true;

	for(CInterpolationMap<CamPathValue>::const_iterator it = m_Map.begin(); it != m_Map.end(); ++it)
	{
		if(selectAll || it->second.Selected)
		{
			CamPathValue curValue = it->second;

			if(first)
			{
				minX = curValue.X;
				minY = curValue.Y;
				minZ = curValue.Z;
				maxX = curValue.X;
				maxY = curValue.Y;
				maxZ = curValue.Z;
				first = false;
			}
			else
			{
				minX = std::min(minX, curValue.X);
				minY = std::min(minY, curValue.Y);
				minZ = std::min(minZ, curValue.Z);
				maxX = std::max(maxX, curValue.X);
				maxY = std::max(maxY, curValue.Y);
				maxZ = std::max(maxZ, curValue.Z);
			}
		}
	}

	double x0 = (maxX +minX) / 2;
	double y0 = (maxY +minY) / 2;
	double z0 = (maxZ +minZ) / 2;

	// build rotation matrix:
	double R[3][3];
	{
		double angle;
		double sr, sp, sy, cr, cp, cy;

		angle = zYaw * (M_PI*2 / 360);
		sy = sin(angle);
		cy = cos(angle);
		angle = yPitch * (M_PI*2 / 360);
		sp = sin(angle);
		cp = cos(angle);
		angle = xRoll * (M_PI*2 / 360);
		sr = sin(angle);
		cr = cos(angle);

		// R = YAW * (PITCH * ROLL)
		R[0][0] = cy*cp;
		R[0][1] = cy*sp*sr -sy*cr;
		R[0][2] = cy*sp*cr +sy*sr;
		R[1][0] = sy*cp;
		R[1][1] = sy*sp*sr +cy*cr;
		R[1][2] = sy*sp*cr +cy*-sr;
		R[2][0] = -sp;
		R[2][1] = cp*sr;
		R[2][2] = cp*cr;
	}
	Quaternion quatR = Quaternion::FromQREulerAngles(QREulerAngles::FromQEulerAngles(QEulerAngles(yPitch, zYaw, xRoll)));

	// rotate:

	for(CInterpolationMap<CamPathValue>::iterator it = m_Map.begin(); it != m_Map.end(); ++it)
	{
		double curT = it->first;
		CamPathValue curValue = it->second;

		if(selectAll || curValue.Selected)
		{
			// update position:
			{
				// translate into origin:
				double x = curValue.X -x0;
				double y = curValue.Y -y0;
				double z = curValue.Z -z0;

				// rotate:
				double Rx = R[0][0]*x +R[0][1]*y +R[0][2]*z;
				double Ry = R[1][0]*x +R[1][1]*y +R[1][2]*z;
				double Rz = R[2][0]*x +R[2][1]*y +R[2][2]*z;

				// translate back:
				curValue.X = Rx +x0;
				curValue.Y = Ry +y0;
				curValue.Z = Rz +z0;
			}

			// update rotation:
			{
				Quaternion quatQ = curValue.R;

				curValue.R = quatR * quatQ;
			}

			// update:
			it->second = curValue;
		}

	}

	m_XInterp->InterpolationMapChanged();
	m_YInterp->InterpolationMapChanged();
	m_ZInterp->InterpolationMapChanged();
	m_RInterp->InterpolationMapChanged();

	Changed();
}

void CamPath::AnchorTransform(double anchorX, double anchorY, double anchorZ, double anchorYPitch, double anchorZYaw, double anchorXRoll, double destX, double destY, double destZ, double destYPitch, double destZYaw, double destXRoll)
{
	ClearCurveData();
	if(m_Map.size()<1) return;

	bool selectAll = true;

	for(CInterpolationMap<CamPathValue>::const_iterator it = m_Map.begin(); it != m_Map.end(); ++it)
	{
		if(it->second.Selected)
		{
			if(selectAll)
			{
				selectAll = false;
				break;
			}
		}
	}

	Quaternion quatAnchor = Quaternion::FromQREulerAngles(QREulerAngles::FromQEulerAngles(QEulerAngles(anchorYPitch, anchorZYaw, anchorXRoll)));
	Quaternion quatDest = Quaternion::FromQREulerAngles(QREulerAngles::FromQEulerAngles(QEulerAngles(destYPitch, destZYaw, destXRoll)));

	// Make sure we take the shortest path:
	double dotProduct = DotProduct(quatDest, quatAnchor);
	if (dotProduct<0.0)
	{
		quatAnchor = -1.0 * quatAnchor;
	}

	Quaternion quatR = quatDest * (/*(1.0 / quatAnchor.Norm()) * */quatAnchor.Conjugate());

	QEulerAngles angles = quatR.ToQREulerAngles().ToQEulerAngles();

	double yPitch = angles.Pitch;
	double zYaw = angles.Yaw;
	double xRoll = angles.Roll;

	// build rotation matrix:
	double R[3][3];
	{
		double angle;
		double sr, sp, sy, cr, cp, cy;

		angle = zYaw * (M_PI*2 / 360);
		sy = sin(angle);
		cy = cos(angle);
		angle = yPitch * (M_PI*2 / 360);
		sp = sin(angle);
		cp = cos(angle);
		angle = xRoll * (M_PI*2 / 360);
		sr = sin(angle);
		cr = cos(angle);

		// R = YAW * (PITCH * ROLL)
		R[0][0] = cy*cp;
		R[0][1] = cy*sp*sr -sy*cr;
		R[0][2] = cy*sp*cr +sy*sr;
		R[1][0] = sy*cp;
		R[1][1] = sy*sp*sr +cy*cr;
		R[1][2] = sy*sp*cr +cy*-sr;
		R[2][0] = -sp;
		R[2][1] = cp*sr;
		R[2][2] = cp*cr;
	}

	// rotate:

	for(CInterpolationMap<CamPathValue>::iterator it = m_Map.begin(); it != m_Map.end(); ++it)
	{
		double curT = it->first;
		CamPathValue curValue = it->second;

		if(selectAll || curValue.Selected)
		{
			// update position:
			{
				// translate into anchor:
				double x = curValue.X -anchorX;
				double y = curValue.Y -anchorY;
				double z = curValue.Z -anchorZ;

				// rotate:
				double Rx = R[0][0]*x +R[0][1]*y +R[0][2]*z;
				double Ry = R[1][0]*x +R[1][1]*y +R[1][2]*z;
				double Rz = R[2][0]*x +R[2][1]*y +R[2][2]*z;

				// translate into destination:
				curValue.X = Rx +destX;
				curValue.Y = Ry +destY;
				curValue.Z = Rz +destZ;
			}

			// update rotation:
			{
				Quaternion quatQ = curValue.R;

				curValue.R = quatR * quatQ;
			}

			// update:
			it->second = curValue;
		}

	}

	m_XInterp->InterpolationMapChanged();
	m_YInterp->InterpolationMapChanged();
	m_ZInterp->InterpolationMapChanged();
	m_RInterp->InterpolationMapChanged();

	Changed();
}

void CamPath::CopyMap(CInterpolationMap<CamPathValue> & dst, CInterpolationMap<CamPathValue> & src)
{
	dst.clear();

	for(CInterpolationMap<CamPathValue>::const_iterator it = src.begin(); it != src.end(); ++it)
	{
		dst[it->first] = it->second;
	}
}

double CamPath::GetDuration() const
{
	if(HasSequenceData()) return GetUpperBound() - GetLowerBound();
	if(HasCurveData()) return GetUpperBound() - GetLowerBound();
	if(m_Map.size()<2) return 0.0;

	return (--m_Map.cend())->first - m_Map.cbegin()->first;
}

void CamPath::SetOffset(double value)
{
	m_Offset = value;

	Changed();
}

double CamPath::GetOffset() const
{
	return m_Offset;
}

void CamPath::OnChangedAdd(CamPathChanged pCamPathChanged, void * pUserData) {
	m_OnChanged.emplace_back(pCamPathChanged,pUserData);
}

void CamPath::OnChangedRemove(CamPathChanged pCamPathChanged, void * pUserData) {
	if(m_OnChangedIt != m_OnChanged.end()) {
		auto it = std::find(m_OnChanged.begin(), m_OnChanged.end(), CamPathChangedData(pCamPathChanged,pUserData));
		if(m_OnChangedIt == it)
			m_OnChangedIt = m_OnChanged.erase(it);
		else
			m_OnChanged.erase(it);
	}
}
