// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

kernel void waste_time(constant uint& cycles [[buffer(0)]], device uint* spin [[buffer(1)]])
{
	uint value = spin[0];
	for (uint i = 0; i < cycles; i++)
		value = spin[value];
	spin[0] = value;
}
