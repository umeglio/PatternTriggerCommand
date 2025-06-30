# 🚀 PatternTriggerCommand Multi-Folder with Web Dashboard

**Author: Umberto Meglio - Development Support: Claude by Anthropic**

![PatternTriggerCommand Dashboard](screenshot.png)

## 🌟 Overview

⚡ PatternTriggerCommand is a next-generation Windows service that monitors multiple folders for files matching regex patterns and automatically executes commands when matches are found. 🤖 The latest version includes a comprehensive web dashboard for real-time monitoring and intelligent system management.

## 💎 Key Features

🔬 **Multi-Folder Monitoring**: Native support for monitoring different folders with specific pattern configurations and advanced detection algorithms

🧠 **Pattern-Based Detection**: Uses cutting-edge regular expressions to identify files of interest with machine-learning-grade matching capabilities

⚙️ **Automated Command Execution**: Triggers customized commands when matching files are detected, passing complete file paths as parameters with intelligent error recovery

🛡️ **File Processing Tracking**: Maintains a persistent database of processed files to prevent redundant processing using enterprise-grade data integrity

📊 **Real-Time Web Dashboard**: Futuristic HTML5 dashboard with live metrics, system monitoring, and predictive performance analytics

🌐 **REST API**: Advanced JSON endpoints for seamless system integration and external monitoring capabilities

📡 **Advanced Metrics Collection**: Tracks memory usage, processing times, thread monitoring, and comprehensive performance analytics with AI-ready data formats

🔮 **Detailed Logging**: Comprehensive activity logs with configurable verbosity levels for debugging and predictive maintenance

🎛️ **Multiple Operation Modes**: Can run as Windows service or console test mode with full web interface access and real-time diagnostics

## 🔥 Technical Specifications

⭐ **Platform**: Windows 7/Server 2008 R2 or higher with modern API support and future-ready architecture
🚀 **Development**: Built in C++ using Windows API with modern C++11 features and performance optimizations
🧠 **Architecture**: Multi-threaded with asynchronous I/O operations for optimal performance and scalability
🌐 **Web Interface**: Integrated HTTP server with responsive dashboard design and mobile-first approach
⚡ **Performance**: Optimized for enterprise environments with real-time metrics collection and predictive analytics
🛡️ **Security**: Thread-safe operations with robust error handling and advanced recovery systems

## 🎯 Quick Start

### ⚙️ Compilation
```bash
# With MinGW in PATH
mingw32-make

# Or specify full path
G:\mingw32\bin\mingw32-make.exe
```

### 🚀 Service Installation
```bash
PatternTriggerCommand.exe install
```

### 🔧 Basic Configuration
🤖 The service automatically creates an intelligent configuration file at `C:\PTC\config.ini`:

```ini
[Settings]
DefaultMonitoredFolder=C:\Monitored
LogFile=C:\PTC\PatternTriggerCommand.log
DetailedLogFile=C:\PTC\PatternTriggerCommand_detailed.log
ProcessedFilesDB=C:\PTC\PatternTriggerCommand_processed.txt
DetailedLogging=true
WebServerPort=8080
WebServerEnabled=true

[Patterns]
# Extended format: Folder|Pattern|Command
Pattern1=C:\Invoices\Incoming|^invoice.*\.pdf$|C:\Scripts\process_invoice.bat
Pattern2=C:\Reports\Monthly|^[0-9]{8}_.*DEMAT.*\.csv$|C:\Scripts\process_demat.bat

# Legacy format: Pattern|Command (uses default folder)
Pattern3=^backup.*\.zip$|C:\Scripts\process_backup.bat
```

## 📊 Web Dashboard Features

🌐 Access the futuristic dashboard at `http://localhost:8080` to monitor:

🔬 **General Statistics**
- Total files processed across all patterns with trend analysis
- Files processed today with intelligent daily counters
- Commands executed with success tracking and performance metrics
- Error counts and failure analysis with predictive insights

⚡ **System Metrics**
- Memory usage in megabytes with trend tracking and optimization suggestions
- Active threads monitoring for performance analysis and bottleneck detection
- Average processing time per file operation with efficiency scoring
- System uptime and service availability with reliability metrics

📡 **Folder Monitoring Status**
- Real-time status of all monitored folders with health indicators
- Files detected versus successfully processed ratios with efficiency analysis
- Active monitoring indicators and intelligent health status assessment
- Pattern-specific performance metrics with optimization recommendations

🧠 **Pattern Analytics**
- Individual pattern match counts and advanced statistics
- Execution success rates and comprehensive failure analysis
- Configuration overview and regex validation with smart suggestions
- Performance optimization recommendations with AI-driven insights

🔮 **Recent Activity Feed**
- Real-time event log with precision timestamps
- Processing notifications and intelligent status updates
- Error reports with detailed diagnostics and resolution suggestions
- System alerts and predictive warning messages

## 💻 Command Line Interface

### 🎛️ Service Management
```bash
PatternTriggerCommand.exe install     # Register as advanced Windows service
PatternTriggerCommand.exe uninstall   # Remove service registration cleanly
PatternTriggerCommand.exe test        # Run in console mode for intelligent testing
PatternTriggerCommand.exe status      # Display comprehensive status with analytics
```

### 💾 Database Operations
```bash
PatternTriggerCommand.exe reset                      # Clear processed files database intelligently
PatternTriggerCommand.exe reprocess <folder> <file>  # Force reprocess with smart validation
```

### ⚙️ Configuration Management
```bash
PatternTriggerCommand.exe config      # Create or update configuration with validation
PatternTriggerCommand.exe config <path> # Use alternative configuration with smart detection
```

## 🔌 REST API Integration

🌐 The service exposes advanced REST endpoints for future-ready system integration:

```bash
GET /                    # Main dashboard HTML interface with responsive design
GET /dashboard           # Alternative dashboard URL with mobile optimization
GET /api/metrics         # Comprehensive JSON system metrics with AI-ready format
```

🤖 **Sample API Response Structure:**
```json
{
  "totalFilesProcessed": 1250,
  "filesProcessedToday": 45,
  "activeThreads": 4,
  "memoryUsageMB": 35,
  "averageProcessingTime": 1200,
  "commandsExecuted": 1200,
  "errorsCount": 3,
  "uptimeSeconds": 86400,
  "lastActivitySeconds": 120,
  "foldersMonitored": 3,
  "patternsConfigured": 5,
  "webServerRunning": true,
  "folders": [...],
  "patterns": [...],
  "recentActivity": [...]
}
```

## 🎯 Pattern Configuration Examples

### 💼 Business Document Processing
```ini
# Invoice processing with intelligent date validation
Pattern1=C:\Enterprise\Invoices|^(invoice|INV)_[0-9]{4}.*\.pdf$|C:\Scripts\process_invoice.bat

# Bank statement integration with smart parsing
Pattern2=C:\Enterprise\Banking|^[0-9]{8}_.*DEMAT.*\.csv$|C:\Scripts\import_banking.bat

# Contract management workflow with automated archiving
Pattern3=C:\Enterprise\Contracts|^contract_.*_signed\.pdf$|C:\Scripts\archive_contract.bat

# Monthly reporting automation with analytics
Pattern4=C:\Enterprise\Reports|^monthly_report_[0-9]{6}\.xlsx$|C:\Scripts\process_report.bat
```

### 🛠️ IT Operations and DevOps
```ini
# Application log analysis with AI insights
Pattern1=C:\Logs\Applications|^app_[0-9]{8}_[0-9]{6}\.log$|C:\Scripts\analyze_logs.bat

# Database backup verification with integrity checking
Pattern2=C:\Backups\Database|^db_backup_.*\.sql\.gz$|C:\Scripts\verify_backup.bat

# Configuration deployment automation with rollback capabilities
Pattern3=C:\Config\Updates|^config_v[0-9]+\.[0-9]+\.xml$|C:\Scripts\deploy_config.bat

# Performance monitoring data processing with predictive analysis
Pattern4=C:\Monitoring\Metrics|^metrics_[0-9]{8}\.json$|C:\Scripts\process_metrics.bat
```

## 🔄 System Operation Flow

🚀 **Initial Scan Phase**: Service performs intelligent comprehensive scan of all configured folders for existing files with smart detection

📡 **Continuous Monitoring**: Real-time filesystem change detection using advanced Windows API notifications for optimal performance

🧠 **Pattern Matching Engine**: Files are evaluated against configured regex patterns with machine-learning-grade accuracy specific to their monitored folder

🛡️ **Duplicate Prevention**: Robust database checking prevents reprocessing with enterprise-grade persistent tracking and integrity validation

⚙️ **Command Execution**: Matched files trigger associated script execution with intelligent timeout management and advanced error handling

📊 **Metrics Collection**: System continuously tracks performance statistics with AI-ready data formats, memory usage, and processing times

🌐 **Web Dashboard Updates**: Live dashboard updates every 2 seconds with real-time system status and predictive activity analysis

## 🛡️ Error Handling and Recovery

🔒 **File Lock Management**: Intelligent waiting system for files to become available with smart retry algorithms before processing attempts

⏱️ **Command Timeout Protection**: Automatic process termination for commands exceeding configured timeout limits with graceful recovery

🔄 **Graceful Recovery**: Service handles unexpected interruptions without data loss using advanced persistent state management

🧵 **Thread Safety**: All operations use sophisticated mutex synchronization for safe concurrent access with deadlock prevention

📝 **Comprehensive Logging**: Multi-level intelligent logging system helps identify and resolve issues with predictive maintenance insights

🧠 **Resource Management**: Automatic cleanup of resources and memory with intelligent periodic maintenance routines and optimization

## 🔍 Troubleshooting Guide

### 🚨 Service Startup Issues
🔧 Verify log folder permissions and available disk space with intelligent diagnostics
⚙️ Check configuration file syntax and validate regex patterns with smart error detection
🛡️ Ensure all monitored folders exist and are accessible by service account with security validation

### 🐛 File Processing Problems
🎯 Test regex patterns using online validation tools for accuracy with smart suggestions
📊 Monitor file detection activity through web dashboard real-time feed with advanced analytics
🔑 Verify script execution permissions and validate file paths with intelligent path resolution

### 🌐 Web Dashboard Access Issues
✅ Confirm WebServerEnabled=true setting in configuration file with smart validation
🔥 Check that WebServerPort is not blocked by Windows firewall with automatic detection
⚡ Ensure configured port is not already in use by other applications with conflict resolution

### 🚀 Performance Optimization
📈 Monitor memory usage and processing times through dashboard with predictive insights
🧵 Review active thread count and optimize pattern complexity with AI-driven recommendations
📊 Analyze error rates and adjust timeout settings with intelligent auto-tuning capabilities

## 🏗️ Build and Development

### ⚙️ Available Make Targets
```bash
mingw32-make            # Compile the complete project with optimizations
mingw32-make install    # Compile and install as Windows service with validation
mingw32-make test       # Compile and run in console test mode with diagnostics
mingw32-make status     # Check current service status with detailed analytics
mingw32-make clean      # Clean all compiled files with intelligent cleanup
mingw32-make reset      # Reset processed files database with integrity check
mingw32-make uninstall  # Uninstall Windows service with complete cleanup
mingw32-make config     # Create or update configuration with smart validation
```

## 🏛️ Technical Architecture

🎛️ **Service Manager**: Handles Windows service lifecycle with enhanced shutdown procedures and intelligent state management

👁️ **Directory Monitor**: Asynchronous multi-folder filesystem monitoring with event-driven architecture and smart filtering

🧠 **Pattern Engine**: Optimized regex engine with performance tracking, compilation caching, and machine-learning-ready optimization

⚡ **Command Executor**: Secure process execution with timeout management, resource control, and intelligent error recovery

💾 **Persistence Layer**: Thread-safe processed files database with atomic operations and enterprise-grade data integrity

🌐 **Web Server**: Integrated HTTP server with REST API, responsive dashboard interface, and future-ready protocols

📊 **Metrics Engine**: Real-time performance analytics with historical data collection and predictive insights

## 🚀 Performance Optimizations

🔄 **Asynchronous I/O Operations**: Non-blocking directory monitoring for maximum efficiency with intelligent resource management

🎯 **Intelligent Pattern Pooling**: Grouping patterns by folder reduces processing overhead with smart optimization algorithms

🧠 **Memory Management**: Optimized caching strategies with automatic cleanup routines and predictive resource allocation

✅ **Graceful Shutdown**: Fast service termination with aggressive 5-second timeout and intelligent state preservation

🧵 **Thread Synchronization**: Efficient mutex-protected operations for concurrent access safety with deadlock prevention

📈 **Metrics Caching**: Smart data collection and serving for responsive dashboard updates with predictive preloading

## 🏢 Enterprise Use Cases

💼 **Business Process Automation**
🤖 Automated invoice processing and approval workflows with intelligent validation
📁 Document management systems with version control and smart archiving
💰 Financial data integration with ERP systems and real-time synchronization
📊 Automated report generation and distribution with predictive scheduling

🛠️ **IT Operations Management**
📝 System log file processing and analysis with AI-powered insights
💾 Automated backup verification and reporting with integrity validation
⚙️ Configuration management and deployment with rollback capabilities
📡 Performance monitoring data collection with predictive maintenance

🔗 **System Integration**
🏭 Legacy system file-based interfaces with intelligent protocol translation
🔄 Data pipeline triggers and processing with smart orchestration
⚡ Workflow automation between disparate systems with adaptive routing
🌐 Real-time data synchronization processes with conflict resolution

## 💻 System Requirements

🖥️ **Operating System**: Windows 7/Server 2008 R2 or higher with full API support and future compatibility
⚒️ **Compiler**: MinGW with C++11 standard library support and optimization capabilities
📚 **Runtime Libraries**: advapi32.dll, ws2_32.dll, psapi.dll (included with Windows) with smart dependency management
🔐 **Permissions**: Administrative privileges required for service installation and intelligent folder monitoring
🌐 **Network Access**: Configurable port (default 8080) for web dashboard and API access with smart firewall detection

## 📜 License and Support

⚖️ **License**: MIT License - See LICENSE file for complete terms and conditions with future-ready licensing

🆘 **Support Resources**:
🌐 Web Dashboard: Real-time system diagnostics at http://localhost:8080 with intelligent troubleshooting
💻 Command Line: Use `PatternTriggerCommand.exe status` for comprehensive system information with AI insights
📝 Detailed Logging: Enable DetailedLogging=true for extensive debugging information with smart filtering
📊 API Monitoring: Use `/api/metrics` endpoint for programmatic system monitoring with predictive analytics

---

*🚀 Next-generation enterprise file automation solution with comprehensive monitoring, real-time visibility, and AI-ready pattern matching capabilities for the digital future.*
