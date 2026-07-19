# SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: CC0-1.0

import pytest
from pytest_embedded import Dut


@pytest.mark.esp32
@pytest.mark.esp32s2
@pytest.mark.esp32s3
@pytest.mark.esp32c3
@pytest.mark.esp32c6
@pytest.mark.esp32h2
@pytest.mark.generic
def test_ai_mirror_generic(dut: Dut) -> None:
    dut.expect('ai_mirror start')
    dut.expect('-----------------------------')
    dut.expect('I \\(([0-9]+)\\) ai_mirror: i2s driver init success')


@pytest.mark.esp32s3
@pytest.mark.generic
@pytest.mark.parametrize(
    'config',
    [
        'bsp',
    ],
    indirect=True,
)
def test_ai_mirror_bsp(dut: Dut) -> None:
    dut.expect('ai_mirror start')
    dut.expect('-----------------------------')
    dut.expect('Using BSP for HW configuration')
    dut.expect('I \\(([0-9]+)\\) ai_mirror: i2s driver init success')
