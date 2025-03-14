/*  PCSX2 - PS2 Emulator for PCs
 *  Copyright (C) 2002-2022  PCSX2 Dev Team
 *
 *  PCSX2 is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU Lesser General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  PCSX2 is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with PCSX2.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include "PrecompiledHeader.h"

#define INITGUID

#include "Frontend/DInputSource.h"
#include "Frontend/InputManager.h"
#include "common/Assertions.h"
#include "common/Console.h"
#include "common/StringUtil.h"

#include "fmt/format.h"
#include <cmath>
#include <limits>

using PFNDIRECTINPUT8CREATE = HRESULT(WINAPI*)(HINSTANCE hinst, DWORD dwVersion, REFIID riidltf, LPVOID* ppvOut, LPUNKNOWN punkOuter);
using PFNGETDFDIJOYSTICK = LPCDIDATAFORMAT(WINAPI*)();

DInputSource::DInputSource() = default;

DInputSource::~DInputSource()
{
	m_controllers.clear();
	m_dinput.reset();
	if (m_dinput_module)
		FreeLibrary(m_dinput_module);
}

std::array<bool, DInputSource::NUM_HAT_DIRECTIONS> DInputSource::GetHatButtons(DWORD hat)
{
	std::array<bool, NUM_HAT_DIRECTIONS> buttons = {};

	const WORD hv = LOWORD(hat);
	if (hv != 0xFFFF)
	{
		if ((hv >= 0 && hv < 9000) || hv >= 31500)
			buttons[HAT_DIRECTION_UP] = true;
		if (hv >= 4500 && hv < 18000)
			buttons[HAT_DIRECTION_RIGHT] = true;
		if (hv >= 13500 && hv < 27000)
			buttons[HAT_DIRECTION_DOWN] = true;
		if (hv >= 22500)
			buttons[HAT_DIRECTION_LEFT] = true;
	}

	return buttons;
}

std::string DInputSource::GetDeviceIdentifier(u32 index)
{
	return fmt::format("DInput-{}", index);
}

static constexpr std::array<const char*, DInputSource::NUM_HAT_DIRECTIONS> s_hat_directions = {{"Up", "Right", "Down", "Left"}};

bool DInputSource::Initialize(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
	m_dinput_module = LoadLibraryW(L"dinput8");
	if (!m_dinput_module)
	{
		Console.Error("Failed to load DInput module.");
		return false;
	}

	PFNDIRECTINPUT8CREATE create = reinterpret_cast<PFNDIRECTINPUT8CREATE>(GetProcAddress(m_dinput_module, "DirectInput8Create"));
	PFNGETDFDIJOYSTICK get_joystick_data_format = reinterpret_cast<PFNGETDFDIJOYSTICK>(GetProcAddress(m_dinput_module, "GetdfDIJoystick"));
	if (!create || !get_joystick_data_format)
	{
		Console.Error("Failed to get DInput function pointers.");
		return false;
	}

	HRESULT hr = create(
		GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8W, reinterpret_cast<LPVOID*>(m_dinput.put()), nullptr);
	m_joystick_data_format = get_joystick_data_format();
	if (FAILED(hr) || !m_joystick_data_format)
	{
		Console.Error("DirectInput8Create() failed: %08X", hr);
		return false;
	}

	// need to release the lock while we're enumerating, because we call winId().
	settings_lock.unlock();
	const std::optional<WindowInfo> toplevel_wi(Host::GetTopLevelWindowInfo());
	if (!toplevel_wi.has_value() || toplevel_wi->type != WindowInfo::Type::Win32)
	{
		Console.Error("Missing top level window, cannot add DInput devices.");
		return false;
	}
	settings_lock.lock();

	m_toplevel_window = static_cast<HWND>(toplevel_wi->window_handle);
	ReloadDevices();
	return true;
}

void DInputSource::UpdateSettings(SettingsInterface& si, std::unique_lock<std::mutex>& settings_lock)
{
	// noop
}

static BOOL CALLBACK EnumCallback(LPCDIDEVICEINSTANCEW lpddi, LPVOID pvRef)
{
	static_cast<std::vector<DIDEVICEINSTANCEW>*>(pvRef)->push_back(*lpddi);
	return DIENUM_CONTINUE;
}

bool DInputSource::ReloadDevices()
{
	// detect any removals
	PollEvents();

	// look for new devices
	std::vector<DIDEVICEINSTANCEW> devices;
	m_dinput->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumCallback, &devices, DIEDFL_ATTACHEDONLY);

	DevCon.WriteLn("Enumerated %zu devices", devices.size());

	bool changed = false;
	for (DIDEVICEINSTANCEW inst : devices)
	{
		// do we already have this one?
		if (std::any_of(
				m_controllers.begin(), m_controllers.end(), [&inst](const ControllerData& cd) { return inst.guidInstance == cd.guid; }))
		{
			// yup, so skip it
			continue;
		}

		ControllerData cd;
		cd.guid = inst.guidInstance;
		HRESULT hr = m_dinput->CreateDevice(inst.guidInstance, cd.device.put(), nullptr);
		if (FAILED(hr))
		{
			Console.Warning("Failed to create instance of device [%s, %s]", inst.tszProductName, inst.tszInstanceName);
			continue;
		}

		const std::string name(StringUtil::WideStringToUTF8String(inst.tszProductName));
		if (AddDevice(cd, name))
		{
			const u32 index = static_cast<u32>(m_controllers.size());
			m_controllers.push_back(std::move(cd));
			Host::OnInputDeviceConnected(GetDeviceIdentifier(index), name);
			changed = true;
		}
	}

	return changed;
}

void DInputSource::Shutdown()
{
	while (!m_controllers.empty())
	{
		Host::OnInputDeviceDisconnected(GetDeviceIdentifier(static_cast<u32>(m_controllers.size() - 1)));
		m_controllers.pop_back();
	}
}

bool DInputSource::AddDevice(ControllerData& cd, const std::string& name)
{
	HRESULT hr = cd.device->SetCooperativeLevel(m_toplevel_window, DISCL_BACKGROUND | DISCL_EXCLUSIVE);
	if (FAILED(hr))
	{
		hr = cd.device->SetCooperativeLevel(m_toplevel_window, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
		if (FAILED(hr))
		{
			Console.Error("Failed to set cooperative level for '%s'", name.c_str());
			return false;
		}

		Console.Warning("Failed to set exclusive mode for '%s'", name.c_str());
	}

	hr = cd.device->SetDataFormat(m_joystick_data_format);
	if (FAILED(hr))
	{
		Console.Error("Failed to set data format for '%s'", name.c_str());
		return false;
	}

	hr = cd.device->Acquire();
	if (FAILED(hr))
	{
		Console.Error("Failed to acquire device '%s'", name.c_str());
		return false;
	}

	DIDEVCAPS caps = {};
	caps.dwSize = sizeof(caps);
	hr = cd.device->GetCapabilities(&caps);
	if (FAILED(hr))
	{
		Console.Error("Failed to get capabilities for '%s'", name.c_str());
		return false;
	}

	cd.num_buttons = caps.dwButtons;

	static constexpr const u32 axis_offsets[] = {offsetof(DIJOYSTATE, lX), offsetof(DIJOYSTATE, lY), offsetof(DIJOYSTATE, lZ),
		offsetof(DIJOYSTATE, lRz), offsetof(DIJOYSTATE, lRx), offsetof(DIJOYSTATE, lRy), offsetof(DIJOYSTATE, rglSlider[0]),
		offsetof(DIJOYSTATE, rglSlider[1])};
	for (const u32 offset : axis_offsets)
	{
		// ask for 16 bits of axis range
		DIPROPRANGE range = {};
		range.diph.dwSize = sizeof(range);
		range.diph.dwHeaderSize = sizeof(range.diph);
		range.diph.dwHow = DIPH_BYOFFSET;
		range.diph.dwObj = static_cast<DWORD>(offset);
		range.lMin = std::numeric_limits<s16>::min();
		range.lMax = std::numeric_limits<s16>::max();
		hr = cd.device->SetProperty(DIPROP_RANGE, &range.diph);

		// did it apply?
		if (SUCCEEDED(cd.device->GetProperty(DIPROP_RANGE, &range.diph)))
			cd.axis_offsets.push_back(offset);
	}

	cd.num_hats = caps.dwPOVs;

	hr = cd.device->Poll();
	if (hr == DI_NOEFFECT)
		cd.needs_poll = false;
	else if (hr != DI_OK)
		Console.Warning("Polling device '%s' failed: %08X", name.c_str(), hr);

	hr = cd.device->GetDeviceState(sizeof(cd.last_state), &cd.last_state);
	if (hr != DI_OK)
		Console.Warning("GetDeviceState() for '%s' failed: %08X", name.c_str(), hr);

	Console.WriteLn(
		"%s has %u buttons, %u axes, %u hats", name.c_str(), cd.num_buttons, static_cast<u32>(cd.axis_offsets.size()), cd.num_hats);

	return (cd.num_buttons > 0 || !cd.axis_offsets.empty() || cd.num_hats > 0);
}

void DInputSource::PollEvents()
{
	for (size_t i = 0; i < m_controllers.size();)
	{
		ControllerData& cd = m_controllers[i];
		if (cd.needs_poll)
			cd.device->Poll();

		DIJOYSTATE js;
		HRESULT hr = cd.device->GetDeviceState(sizeof(js), &js);
		if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED)
		{
			hr = cd.device->Acquire();
			if (hr == DI_OK)
				hr = cd.device->GetDeviceState(sizeof(js), &js);

			if (hr != DI_OK)
			{
				Host::OnInputDeviceDisconnected(GetDeviceIdentifier(static_cast<u32>(i)));
				m_controllers.erase(m_controllers.begin() + i);
				continue;
			}
		}
		else if (hr != DI_OK)
		{
			Console.Warning("GetDeviceState() failed: %08X", hr);
			i++;
			continue;
		}

		CheckForStateChanges(i, js);
		i++;
	}
}

std::vector<std::pair<std::string, std::string>> DInputSource::EnumerateDevices()
{
	std::vector<std::pair<std::string, std::string>> ret;
	for (size_t i = 0; i < m_controllers.size(); i++)
	{
		DIDEVICEINSTANCEW dii = {sizeof(DIDEVICEINSTANCEW)};
		std::string name;
		if (SUCCEEDED(m_controllers[i].device->GetDeviceInfo(&dii)))
			name = StringUtil::WideStringToUTF8String(dii.tszProductName);

		if (name.empty())
			name = "Unknown";

		ret.emplace_back(GetDeviceIdentifier(static_cast<u32>(i)), std::move(name));
	}

	return ret;
}

std::vector<InputBindingKey> DInputSource::EnumerateMotors()
{
	return {};
}

bool DInputSource::GetGenericBindingMapping(const std::string_view& device, GenericInputBindingMapping* mapping)
{
	return {};
}

void DInputSource::UpdateMotorState(InputBindingKey key, float intensity)
{
	// not supported
}

void DInputSource::UpdateMotorState(InputBindingKey large_key, InputBindingKey small_key, float large_intensity, float small_intensity)
{
	// not supported
}

std::optional<InputBindingKey> DInputSource::ParseKeyString(const std::string_view& device, const std::string_view& binding)
{
	if (!StringUtil::StartsWith(device, "DInput-") || binding.empty())
		return std::nullopt;

	const std::optional<s32> player_id = StringUtil::FromChars<s32>(device.substr(7));
	if (!player_id.has_value() || player_id.value() < 0)
		return std::nullopt;

	InputBindingKey key = {};
	key.source_type = InputSourceType::DInput;
	key.source_index = static_cast<u32>(player_id.value());

	if (StringUtil::StartsWith(binding, "+Axis") || StringUtil::StartsWith(binding, "-Axis"))
	{
		const std::optional<u32> axis_index = StringUtil::FromChars<u32>(binding.substr(5));
		if (!axis_index.has_value())
			return std::nullopt;

		key.source_subtype = InputSubclass::ControllerAxis;
		key.data = axis_index.value();
		key.negative = (binding[0] == '-');
		return key;
	}
	else if (StringUtil::StartsWith(binding, "Hat"))
	{
		if (binding[3] < '0' || binding[3] > '9' || binding.length() < 5)
			return std::nullopt;

		const u32 hat_index = binding[3] - '0';
		const std::string_view hat_dir(binding.substr(4));
		for (u32 i = 0; i < NUM_HAT_DIRECTIONS; i++)
		{
			if (hat_dir == s_hat_directions[i])
			{
				key.source_subtype = InputSubclass::ControllerButton;
				key.data = MAX_NUM_BUTTONS + hat_index * NUM_HAT_DIRECTIONS + i;
				return key;
			}
		}

		// bad direction
		return std::nullopt;
	}
	else if (StringUtil::StartsWith(binding, "Button"))
	{
		const std::optional<u32> button_index = StringUtil::FromChars<u32>(binding.substr(6));
		if (!button_index.has_value())
			return std::nullopt;

		key.source_subtype = InputSubclass::ControllerButton;
		key.data = button_index.value();
		return key;
	}

	// unknown axis/button
	return std::nullopt;
}

std::string DInputSource::ConvertKeyToString(InputBindingKey key)
{
	std::string ret;

	if (key.source_type == InputSourceType::DInput)
	{
		if (key.source_subtype == InputSubclass::ControllerAxis)
		{
			ret = fmt::format("DInput-{}/{}Axis{}", u32(key.source_index), key.negative ? '-' : '+', u32(key.data));
		}
		else if (key.source_subtype == InputSubclass::ControllerButton && key.data >= MAX_NUM_BUTTONS)
		{
			const u32 hat_num = (key.data - MAX_NUM_BUTTONS) / NUM_HAT_DIRECTIONS;
			const u32 hat_dir = (key.data - MAX_NUM_BUTTONS) % NUM_HAT_DIRECTIONS;
			ret = fmt::format("DInput-{}/Hat{}{}", u32(key.source_index), hat_num, s_hat_directions[hat_dir]);
		}
		else if (key.source_subtype == InputSubclass::ControllerButton)
		{
			ret = fmt::format("DInput-{}/Button{}", u32(key.source_index), u32(key.data));
		}
	}

	return ret;
}

void DInputSource::CheckForStateChanges(size_t index, const DIJOYSTATE& new_state)
{
	ControllerData& cd = m_controllers[index];
	DIJOYSTATE& last_state = cd.last_state;

	for (size_t i = 0; i < cd.axis_offsets.size(); i++)
	{
		LONG new_value;
		LONG old_value;
		std::memcpy(&old_value, reinterpret_cast<const u8*>(&cd.last_state) + cd.axis_offsets[i], sizeof(old_value));
		std::memcpy(&new_value, reinterpret_cast<const u8*>(&new_state) + cd.axis_offsets[i], sizeof(new_value));
		if (old_value != new_value)
		{
			std::memcpy(reinterpret_cast<u8*>(&cd.last_state) + cd.axis_offsets[i], &new_value, sizeof(new_value));

			// TODO: Use the range from caps?
			const float value = static_cast<float>(new_value) / (new_value < 0 ? 32768.0f : 32767.0f);
			InputManager::InvokeEvents(MakeGenericControllerAxisKey(InputSourceType::DInput, static_cast<u32>(index), static_cast<u32>(i)),
				value, GenericInputBinding::Unknown);
		}
	}

	for (u32 i = 0; i < cd.num_buttons; i++)
	{
		if (last_state.rgbButtons[i] != new_state.rgbButtons[i])
		{
			last_state.rgbButtons[i] = new_state.rgbButtons[i];

			const float value = (new_state.rgbButtons[i] != 0) ? 1.0f : 0.0f;
			InputManager::InvokeEvents(
				MakeGenericControllerButtonKey(InputSourceType::DInput, static_cast<u32>(index), i), value, GenericInputBinding::Unknown);
		}
	}

	for (u32 i = 0; i < cd.num_hats; i++)
	{
		if (last_state.rgdwPOV[i] != new_state.rgdwPOV[i])
		{
			// map hats to the last buttons
			const std::array<bool, NUM_HAT_DIRECTIONS> old_buttons(GetHatButtons(last_state.rgdwPOV[i]));
			const std::array<bool, NUM_HAT_DIRECTIONS> new_buttons(GetHatButtons(new_state.rgdwPOV[i]));
			last_state.rgdwPOV[i] = new_state.rgdwPOV[i];

			for (u32 j = 0; j < NUM_HAT_DIRECTIONS; j++)
			{
				if (old_buttons[j] != new_buttons[j])
				{
					const float value = (new_buttons[j] ? 1.0f : 0.0f);
					InputManager::InvokeEvents(MakeGenericControllerButtonKey(InputSourceType::DInput, static_cast<u32>(index),
												   cd.num_buttons + (i * NUM_HAT_DIRECTIONS) + j),
						value, GenericInputBinding::Unknown);
				}
			}
		}
	}
}
