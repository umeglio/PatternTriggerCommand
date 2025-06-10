# 🎯 PatternTriggerCommand Multi-Folder

*Author: Umberto Meglio - Development Support: Claude by Anthropic*

## 🌟 Project Overview

PatternTriggerCommand is an advanced Windows service application that monitors multiple folders simultaneously for new or modified files matching configurable regex patterns. When a matching file is detected, the service automatically executes associated command scripts or applications, passing the complete file path as a parameter.

This service represents a flexible middleware solution designed to bridge the gap between file system events and application processes in Windows environments, providing enterprise-grade automation for document workflows and data processing pipelines.

### ⚡ Key Features

- **🗂️ Multi-Folder Monitoring**: Native support for monitoring different folders with specific patterns
- **🎯 Pattern-Based Monitoring**: Uses regular expressions to identify files of interest
- **⚙️ Automated Command Execution**: Triggers customized commands when matching files are detected
- **🧠 File Processing Tracking**: Maintains a database of processed files to prevent redundant processing
- **⚙️ Configuration Flexibility**: Supports customizable monitoring paths, log settings, and pattern-command pairs
- **📝 Detailed Logging**: Provides comprehensive activity logs with configurable verbosity
- **🔄 Multiple Operation Modes**: Can run as a Windows service or in console test mode

### 🏗️ Technical Specifications

- **💻 Development**: Built in C++ using the Windows API
- **⚡ Performance**: Supports Windows directory change notifications for efficient monitoring
- **🔄 Architecture**: Implements asynchronous I/O operations for responsive file handling
- **🛡️ Reliability**: Features a robust error handling and recovery system
- **🔒 Safety**: Includes file locking detection to prevent processing in-use files
- **📊 Scalability**: Optimized for enterprise environments with multiple concurrent folder monitoring

## 🚀 Quick Installation

### ⚒️ Compilation

```bash
# With MinGW
G:\mingw32\bin\mingw32-make.exe

# Or if MinGW is in PATH
mingw32-make
```

### 📦 Service Installation

```bash
PatternTriggerCommand.exe install
```

### ⚙️ Basic Configuration

The service automatically creates a sample configuration file at `C:\PTC\config.ini` on first startup.

## 🛠️ Advanced Configuration

### 📋 Multi-Folder Configuration Format

The `C:\PTC\config.ini` file now supports two formats for maximum flexibility:

```ini
[Settings]
DefaultMonitoredFolder=C:\Monitored
LogFile=C:\PTC\PatternTriggerCommand.log
DetailedLogFile=C:\PTC\PatternTriggerCommand_detailed.log
ProcessedFilesDB=C:\PTC\PatternTriggerCommand_processed.txt
DetailedLogging=true

[Patterns]
# Extended format: Folder|Pattern|Command
Pattern1=C:\Invoices\Incoming|^invoice.*\.pdf$|C:\Scripts\process_invoice.bat
Pattern2=C:\Reports\Monthly|^[0-9]{8}_.*DEMAT.*\.csv$|C:\Scripts\process_demat.bat
Pattern3=C:\Documents\Contracts|^contract.*\.docx?$|C:\Scripts\process_contract.bat

# Compatible format: Pattern|Command (uses default folder)
Pattern4=^backup.*\.zip$|C:\Scripts\process_backup.bat
```

### 🎨 Regex Pattern Examples

```ini
# Files with date and specific text
Pattern1=C:\Data|^[0-9]{8}_.*DEMAT.*\.csv$|C:\Scripts\process_demat.bat

# Invoice PDFs
Pattern2=C:\Invoices|^(invoice|fattura).*\.pdf$|C:\Scripts\process_invoice.bat

# Office documents
Pattern3=C:\Docs|^report.*\.(xlsx?|docx?)$|C:\Scripts\process_office.bat

# Timestamped files
Pattern4=C:\Logs|^log_[0-9]{4}-[0-9]{2}-[0-9]{2}.*\.txt$|C:\Scripts\process_log.bat
```

## 💻 Command-Line Interface

### 🔧 Service Management

```bash
PatternTriggerCommand.exe install        # Register as a Windows service
PatternTriggerCommand.exe uninstall      # Remove the service registration
PatternTriggerCommand.exe test           # Run in console mode for testing
PatternTriggerCommand.exe status         # Display service status and configuration details
```

### 💾 Database Management

```bash
PatternTriggerCommand.exe reset          # Clear the processed files database
PatternTriggerCommand.exe reprocess <folder> <file>  # Force reprocessing of a specific file
```

### ⚙️ Configuration Management

```bash
PatternTriggerCommand.exe config         # Create or update the configuration file
PatternTriggerCommand.exe config <path>  # Use alternative configuration file
```

### 🏗️ Makefile Targets

```bash
mingw32-make                    # Compile the project
mingw32-make install           # Compile and install service
mingw32-make test              # Compile and start test mode
mingw32-make status            # Check service status
mingw32-make clean             # Clean compiled files
mingw32-make reset             # Reset processed files database
mingw32-make uninstall         # Uninstall service
mingw32-make config            # Update configuration
```

## ⚙️ System Operation

### 🔄 Processing Flow

1. **🔍 Initial Scan**: The service scans all configured folders for existing files
2. **👁️ Continuous Monitoring**: Uses Windows APIs to detect filesystem changes in real-time
3. **🎯 Pattern Matching**: Compares filenames against all regex patterns configured for the specific folder
4. **✅ Duplicate Verification**: Checks database to prevent reprocessing
5. **🚀 Command Execution**: Launches associated script/command passing the complete file path
6. **📊 Tracking**: Updates the processed files database

### 🛡️ Error Handling

- **🔒 File In Use**: The service waits for files to become available before processing
- **⏱️ Command Timeout**: Automatically terminates processes that exceed configured timeout
- **🔄 Graceful Recovery**: Handles service interruptions without data loss
- **📝 Extended Logging**: Records all events to facilitate debugging

## 🏢 Practical Examples

### 📊 Enterprise Scenario: Document Management

```ini
[Patterns]
# Incoming invoices
Pattern1=C:\Company\Invoices\Incoming|^(invoice|fattura)_[0-9]{4}.*\.pdf$|C:\Scripts\process_invoice.bat

# Bank statements
Pattern2=C:\Company\Banking|^[0-9]{8}_.*DEMAT.*\.csv$|C:\Scripts\import_banking.bat

# Signed contracts
Pattern3=C:\Company\Contracts\Signed|^contract_.*_signed\.pdf$|C:\Scripts\archive_contract.bat

# Monthly reports
Pattern4=C:\Company\Reports|^monthly_report_[0-9]{6}\.xlsx$|C:\Scripts\process_report.bat
```

### 💻 Technical Scenario: Data Processing

```ini
[Patterns]
# Application logs
Pattern1=C:\Logs\Applications|^app_[0-9]{8}_[0-9]{6}\.log$|C:\Scripts\analyze_logs.bat

# Database backups
Pattern2=C:\Backups\Database|^db_backup_.*\.sql\.gz$|C:\Scripts\verify_backup.bat

# Configuration files
Pattern3=C:\Config\Updates|^config_v[0-9]+\.[0-9]+\.xml$|C:\Scripts\deploy_config.bat
```

## 📊 Monitoring And Troubleshooting

### 📄 Log Files

- **📋 Main Log**: `C:\PTC\PatternTriggerCommand.log` - Main events
- **🔍 Detailed Log**: `C:\PTC\PatternTriggerCommand_detailed.log` - Complete debug information
- **💾 Processed Database**: `C:\PTC\PatternTriggerCommand_processed.txt` - Already processed files

### 🔧 Diagnostic Commands

```bash
# Check complete status
PatternTriggerCommand.exe status

# Test configuration without installing service
PatternTriggerCommand.exe test

# Verify specific patterns
PatternTriggerCommand.exe reprocess "C:\TestFolder" "testfile.pdf"
```

### 🚨 Common Issue Resolution

**❌ Service Won't Start**
- Verify log folder permissions
- Check configuration file syntax
- Verify existence of monitored folders

**🚫 Files Not Being Processed**
- Test regex patterns with online tools
- Verify script execution permissions
- Check detailed log for specific errors

**⚡ Performance Degradation**
- Reduce number of monitored folders
- Optimize regex patterns
- Increase control intervals in code

## 💻 System Requirements

- **🖥️ Operating System**: Windows 7/Server 2008 or higher
- **⚒️ Compiler**: MinGW with C++11 support
- **📚 Libraries**: advapi32.dll (included in Windows)
- **🔐 Permissions**: Administrative privileges for service installation

## 🏗️ Technical Architecture

### 🔧 Core Components

- **🎛️ Service Manager**: Windows service lifecycle management
- **👁️ Directory Monitor**: Asynchronous multi-folder filesystem monitoring
- **🎯 Pattern Engine**: Regex engine for file matching
- **⚡ Command Executor**: Controlled execution of external processes
- **💾 Persistence Layer**: Processed files database management

### ⚡ Implemented Optimizations

- **🔄 Asynchronous I/O**: Non-blocking directory monitoring
- **🎯 Intelligent Pooling**: Pattern grouping by folder
- **🧠 Efficient Caching**: Memory management for compiled patterns
- **✅ Graceful Shutdown**: Clean termination with state saving

## 🎯 Use Cases

### 📈 Business Applications

- **📋 Document Processing Workflows**: Automatic processing of incoming business documents
- **🔄 Automated File Conversion Systems**: Converting files between different formats
- **🔗 Integration Points**: Bridging disparate systems through file-based triggers
- **⚡ Business Process Triggers**: Initiating workflows based on file appearance
- **📊 Batch Processing**: Enterprise data file processing in controlled environments

### 🏭 Technical Applications

- **📝 Log Analysis**: Automatic processing of application and system logs
- **💾 Backup Automation**: Triggering backup verification and archival processes
- **⚙️ Configuration Management**: Automated deployment of configuration updates
- **📊 Data Pipeline Integration**: File-based ETL process initiation
- **🔍 Quality Assurance**: Automated testing trigger based on file delivery

## 🤝 Contributing To The Project

The project welcomes contributions for improvements and new features. Areas of interest:

- 📁 Support for more complex patterns (multi-level directory)
- 📢 Integration with external notification systems
- 📊 Web dashboard for real-time monitoring
- 🎯 Support for multiple actions per single pattern
- 🔧 Advanced filtering and conditional processing
- 📈 Performance monitoring and metrics collection

## 📜 License

MIT License - See `LICENSE` file for complete details.

## 🆘 Support And Documentation

For technical assistance and additional documentation:
- Use the `status` command for automatic diagnostics
- Enable `DetailedLogging=true` for thorough debugging
- Consult log files for specific error analysis
- Test patterns in isolation using the `test` mode

## 🔧 Best Practices

### 📋 Configuration Guidelines

- **🎯 Pattern Specificity**: Use specific patterns to avoid false matches
- **📁 Folder Organization**: Group related patterns by business function
- **⚡ Performance**: Minimize the number of monitored folders for optimal performance
- **🔒 Security**: Ensure script execution permissions are properly configured

### 🛡️ Security Considerations

- **🔐 Script Permissions**: Run scripts with minimum required privileges
- **📁 Folder Access**: Restrict access to monitored folders
- **📝 Log Security**: Protect log files from unauthorized access
- **🔄 Service Account**: Use dedicated service account with limited privileges

---

*🎯 PatternTriggerCommand Multi-Folder represents a robust evolution for enterprise document workflow automation, combining configuration flexibility with enterprise-grade performance and reliability.*
