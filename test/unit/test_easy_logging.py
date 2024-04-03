#
# Copyright (c) 2012-2023 Snowflake Computing Inc. All rights reserved.
#

from __future__ import annotations

import os.path

from snowflake.connector.log_configuration import EasyLoggingConfigPython

config_file_path = os.path.join(
    os.getcwd(), "..", "data", "config_files", "sf_client_config.json"
)
wrong_common_config_file_path = os.path.join(
    os.getcwd(), "..", "data", "config_files", "wrong_common_config_file.json"
)
wrong_log_level_config_file_path = os.path.join(
    os.getcwd(), "..", "data", "config_files", "wrong_log_level_config_file.json"
)
wrong_log_path_level_config_file_path = os.path.join(
    os.getcwd(), "..", "data", "config_files", "wrong_log_path_level_config_file.json"
)
incomplete_config_file_path = os.path.join(
    "..", "data", "config_files", "sf_client_config.json"
)
FAKE_CONNECTION_PARAMETERS = {"CLIENT_CONFIG_FILE": config_file_path}
FAKE_CONNECTION_PARAMETERS_WITHOUT_FULL_PATH = {
    "CLIENT_CONFIG_FILE": incomplete_config_file_path
}


# read from home dir and drive dir will not be tested because it requires to create file under those dirs
def test_parse_config_file_with_connection_parameter():
    config = EasyLoggingConfigPython(FAKE_CONNECTION_PARAMETERS)
    assert (
        config.CLIENT_CONFIG_FILE == config_file_path
        and config.SF_CLIENT_CONFIG_FILE is None
    )
    assert config.log_level == "DEBUG"
    assert config.log_path == "../data"


def test_parse_config_file_with_environment_variable():
    os.environ["SF_CLIENT_CONFIG_FILE"] = os.path.join(
        os.getcwd(), "..", "data", "config_files", "sf_client_config.json"
    )
    config = EasyLoggingConfigPython()
    assert (
        config.CLIENT_CONFIG_FILE is None
        and config.SF_CLIENT_CONFIG_FILE == config_file_path
    )
    assert config.log_level == "DEBUG"
    assert config.log_path == "../data"


def test_parse_config_file_with_no_config_file():
    config = EasyLoggingConfigPython()
    assert config.CLIENT_CONFIG_FILE is None and config.SF_CLIENT_CONFIG_FILE is None
    assert config.log_level is None
    assert config.log_path is None


def test_config_file_errors():
    try:
        EasyLoggingConfigPython(FAKE_CONNECTION_PARAMETERS_WITHOUT_FULL_PATH)
    except FileNotFoundError as e:
        assert f"given file path {incomplete_config_file_path} is not full path" in str(
            e
        )

    try:
        EasyLoggingConfigPython({"CLIENT_CONFIG_FILE": wrong_common_config_file_path})
    except ValueError as e:
        assert (
            f"config file at {wrong_common_config_file_path} is not in correct form, please verify your config file"
            in str(e)
        )

    try:
        EasyLoggingConfigPython(
            {"CLIENT_CONFIG_FILE": wrong_log_path_level_config_file_path}
        )
    except PermissionError as e:
        assert "log path: ../not_exist is not accessible" in str(e)

    try:
        EasyLoggingConfigPython(
            {"CLIENT_CONFIG_FILE": wrong_log_level_config_file_path}
        )
    except ValueError as e:
        assert "given log level: wrong is not valid" in str(e)
