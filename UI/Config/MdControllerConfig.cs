using Mesen.Interop;
using Mesen.Localization;
using Mesen.ViewModels;
using System;
using System.Collections.Generic;
using System.Linq;

namespace Mesen.Config
{
	public class MdControllerConfig : ControllerConfig
	{
		public new MdKeyMapping Mapping1 { get => (MdKeyMapping)_mapping1; set => _mapping1 = value; }
		public new MdKeyMapping Mapping2 { get => (MdKeyMapping)_mapping2; set => _mapping2 = value; }
		public new MdKeyMapping Mapping3 { get => (MdKeyMapping)_mapping3; set => _mapping3 = value; }
		public new MdKeyMapping Mapping4 { get => (MdKeyMapping)_mapping4; set => _mapping4 = value; }

		public MdControllerConfig()
		{
			_mapping1 = new MdKeyMapping();
			_mapping2 = new MdKeyMapping();
			_mapping3 = new MdKeyMapping();
			_mapping4 = new MdKeyMapping();
		}
	}

	//Custom buttons of the Sega Mouse and the light guns (Core/MD/Input/MdMouse.h, MdLightGun.h)
	public class MdKeyMapping : KeyMapping
	{
		public UInt16[]? MouseButtons { get; set; } = null;
		public UInt16[]? MenacerButtons { get; set; } = null;
		public UInt16[]? JustifierButtons { get; set; } = null;

		protected override UInt16[]? GetCustomButtons(ControllerType type)
		{
			return type switch {
				ControllerType.MdMouse => MouseButtons,
				ControllerType.MdMenacer => MenacerButtons,
				ControllerType.MdJustifier => JustifierButtons,
				_ => null
			};
		}

		public override List<CustomKeyMapping> ToCustomKeys(ControllerType type, int mappingIndex)
		{
			UInt16[]? buttonMappings = GetCustomButtons(type);
			if(buttonMappings == null) {
				if(GetDefaultCustomKeys(type, null) != null) {
					if(mappingIndex == 0) {
						SetDefaultKeys(type, null);
					} else {
						ClearKeys(type);
					}
				}

				buttonMappings = GetCustomButtons(type);
				if(buttonMappings == null) {
					return new List<CustomKeyMapping>();
				}
			}

			return type switch {
				ControllerType.MdMouse => Enum.GetValues<MdMouseButtons>().Select(val => new CustomKeyMapping(ResourceHelper.GetEnumText(val), buttonMappings, (int)val)).ToList(),
				ControllerType.MdMenacer => Enum.GetValues<MdMenacerButtons>().Select(val => new CustomKeyMapping(ResourceHelper.GetEnumText(val), buttonMappings, (int)val)).ToList(),
				ControllerType.MdJustifier => Enum.GetValues<MdJustifierButtons>().Select(val => new CustomKeyMapping(ResourceHelper.GetEnumText(val), buttonMappings, (int)val)).ToList(),
				_ => new()
			};
		}

		public override void ClearKeys(ControllerType type)
		{
			switch(type) {
				case ControllerType.MdMouse: MouseButtons = new UInt16[4]; break;
				case ControllerType.MdMenacer: MenacerButtons = new UInt16[4]; break;
				case ControllerType.MdJustifier: JustifierButtons = new UInt16[2]; break;
				default: base.ClearKeys(type); break;
			}
		}

		public override UInt16[]? GetDefaultCustomKeys(ControllerType type, KeyPresetType? preset)
		{
			switch(type) {
				case ControllerType.MdMouse:
					return new UInt16[4] {
						InputApi.GetKeyCode("Mouse Left"),
						InputApi.GetKeyCode("Mouse Right"),
						InputApi.GetKeyCode("Mouse Middle"),
						InputApi.GetKeyCode("Enter"),
					};

				case ControllerType.MdMenacer:
					return new UInt16[4] {
						InputApi.GetKeyCode("Mouse Left"),
						InputApi.GetKeyCode("Enter"),
						InputApi.GetKeyCode("Mouse Right"),
						InputApi.GetKeyCode("Mouse Middle"),
					};

				case ControllerType.MdJustifier:
					return new UInt16[2] {
						InputApi.GetKeyCode("Mouse Left"),
						InputApi.GetKeyCode("Enter"),
					};

				default:
					return null;
			}
		}

		public override void SetDefaultKeys(ControllerType type, KeyPresetType? preset)
		{
			switch(type) {
				case ControllerType.MdMouse: MouseButtons = GetDefaultCustomKeys(type, preset); break;
				case ControllerType.MdMenacer: MenacerButtons = GetDefaultCustomKeys(type, preset); break;
				case ControllerType.MdJustifier: JustifierButtons = GetDefaultCustomKeys(type, preset); break;
				default: base.SetDefaultKeys(type, preset); break;
			}
		}
	}

	//Indexes match the CustomKeys order read by the core devices
	public enum MdMouseButtons { Left, Right, Middle, Start };
	public enum MdMenacerButtons { Trigger, Start, A, C };
	public enum MdJustifierButtons { Trigger, Start };
}
