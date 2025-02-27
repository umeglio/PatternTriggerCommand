# PatternTriggerCommand

PatternTriggerCommand Project Description

Overview
The PatternTriggerCommand is a Windows service application that monitors specified folders for new or modified files matching configurable regex patterns. When a matching file is detected, the service automatically executes associated command scripts or applications, passing the file path as a parameter.
Key Features

Pattern-Based Monitoring: Uses regular expressions to identify files of interest
Automated Command Execution: Triggers customized commands when matching files are detected
File Processing Tracking: Maintains a database of processed files to prevent redundant processing
Configuration Flexibility: Supports customizable monitoring paths, log settings, and pattern-command pairs
Detailed Logging: Provides comprehensive activity logs with configurable verbosity
Multiple Operation Modes: Can run as a Windows service or in console test mode

Technical Specifications

Developed in C++ using the Windows API
Supports Windows directory change notifications for efficient monitoring
Implements asynchronous I/O operations for responsive file handling
Features a robust error handling and recovery system
Includes file locking detection to prevent processing in-use files

Command-Line Interface
The application supports multiple command-line operations:

install: Register as a Windows service
uninstall: Remove the service registration
test: Run in console mode for testing
status: Display service status and configuration details
reset: Clear the processed files database
reprocess [filename]: Force reprocessing of a specific file
config: Create or update the configuration file

Configuration
The service is configured via an INI file with two main sections:

Settings: Define monitoring paths, log files, and general options
Patterns: Specify regex patterns and associated command mappings

Use Cases

Document processing workflows
Automated file conversion systems
Integration points between disparate systems
Triggering business processes based on file appearance
Batch processing for data files in enterprise environments

This service represents a flexible middleware solution designed to bridge the gap between file system events and application processes in Windows environments.
