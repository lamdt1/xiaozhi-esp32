# Product Brief: xiaozhi-esp32

**Date:** 2025-11-15
**Author:** Robert
**Context:** Open-source hardware/software project

---

## Executive Summary

Xiaozhi ESP32 is an open-source AI voice assistant platform that enables developers, makers, and hobbyists to build intelligent voice interaction devices using ESP32 microcontrollers. The project combines offline voice wake-up, streaming ASR/LLM/TTS processing, and the MCP (Model Context Protocol) to create a flexible, extensible AI hardware platform that supports 70+ different ESP32-based development boards. Unlike proprietary voice assistant solutions, Xiaozhi ESP32 provides complete source code, customizable assets, and a protocol-based architecture that allows users to extend AI capabilities through both device-side and cloud-side MCP servers.

The platform addresses the gap between powerful cloud-based AI services and accessible, customizable embedded hardware, enabling anyone to build personalized AI devices without vendor lock-in or expensive proprietary hardware.

---

## Core Vision

### Problem Statement

The AI voice assistant market is dominated by proprietary, closed ecosystems (Amazon Alexa, Google Assistant, Apple Siri) that offer limited customization, require cloud dependencies, and lock users into specific hardware platforms. Developers and makers who want to create custom AI voice interfaces face several critical barriers:

1. **Hardware Lock-in**: Commercial voice assistants are tied to specific hardware platforms with no portability
2. **Limited Customization**: Users cannot modify wake words, UI elements, or core behaviors
3. **Cloud Dependency**: Most solutions require constant internet connectivity and send all audio to third-party servers
4. **High Cost**: Proprietary development kits and platforms are expensive ($100-300+)
5. **Protocol Limitations**: Existing solutions don't provide extensible protocols for device control or AI capability expansion
6. **Language Barriers**: Many solutions have limited multi-language support, especially for Chinese, Japanese, and other non-English markets

### Problem Impact

- **Makers and Hobbyists**: Cannot build personalized AI devices without significant technical expertise or budget
- **Developers**: Face vendor lock-in when building IoT products with voice interfaces
- **Hardware Manufacturers**: Limited to expensive proprietary solutions or building custom stacks from scratch
- **Non-English Markets**: Lack of accessible, customizable voice AI solutions for Chinese, Japanese, and other language speakers
- **Privacy-Conscious Users**: Forced to choose between functionality and data privacy

The cost of building a custom voice AI device from scratch can take 6-12 months of development time and $50,000-200,000 in engineering resources. Even then, developers face ongoing maintenance, cloud service costs, and integration challenges.

### Why Existing Solutions Fall Short

**Commercial Voice Assistants (Alexa, Google, Siri)**:
- No source code access or customization
- Vendor lock-in to specific ecosystems
- Limited hardware platform support
- Privacy concerns with cloud processing
- High ongoing service costs

**Proprietary Development Kits**:
- Expensive ($100-500+ per unit)
- Limited to specific hardware configurations
- Closed-source firmware
- No community-driven extensions
- Vendor-dependent support

**DIY Solutions (Raspberry Pi + open-source stacks)**:
- High power consumption (not battery-friendly)
- Complex setup requiring Linux expertise
- Larger form factor unsuitable for wearable/portable devices
- Higher BOM cost ($50-100+)
- Not optimized for real-time voice processing

**ESP32 Voice Projects**:
- Typically single-board implementations
- No standardized protocol for extensibility
- Limited multi-language support
- No unified asset management system
- Fragmented community efforts

### Proposed Solution

Xiaozhi ESP32 is a complete, open-source firmware platform that transforms any ESP32-based development board into an intelligent voice assistant. The solution provides:

1. **Universal Hardware Support**: Single codebase supporting 70+ ESP32 boards (ESP32-C3, ESP32-S3, ESP32-P4) with board-specific configurations
2. **Offline Voice Wake-up**: On-device wake word detection using ESP-SR, eliminating constant cloud connectivity
3. **Streaming AI Pipeline**: Real-time ASR (Automatic Speech Recognition) → LLM (Large Language Model) → TTS (Text-to-Speech) processing with low latency
4. **MCP Protocol Integration**: Extensible architecture allowing device control (GPIO, LEDs, motors) and cloud AI capabilities (smart home, PC control, knowledge search) through standardized MCP servers
5. **Customizable Assets**: Web-based tool for generating custom wake words, fonts, emojis, and UI backgrounds
6. **Multi-Protocol Support**: Both WebSocket and MQTT+UDP communication options for different deployment scenarios
7. **Multi-Language Support**: Native support for Chinese, English, and Japanese with extensible language framework
8. **Complete Open Source**: MIT-licensed codebase enabling commercial use, modification, and distribution

### Key Differentiators

1. **True Hardware Portability**: One firmware codebase runs on 70+ different ESP32 boards, from $5 breadboard setups to $50 commercial development kits
2. **MCP-First Architecture**: First voice assistant platform built on Model Context Protocol, enabling unprecedented extensibility for both device control and AI capabilities
3. **Offline-First Design**: Wake word detection runs entirely on-device, reducing power consumption and enabling battery-powered applications
4. **Community-Driven Hardware Support**: Open board configuration system allows community to add new hardware without modifying core firmware
5. **Asset Customization System**: Web-based generator enables non-technical users to customize visual and audio assets without recompiling firmware
6. **Multi-Protocol Flexibility**: Supports both WebSocket (for direct server connections) and MQTT+UDP (for IoT deployments), giving developers deployment flexibility
7. **Open Ecosystem**: MIT license enables commercial use, creating a sustainable open-source model that benefits both hobbyists and commercial developers

---

## Target Users

### Primary Users

**1. Maker Community & Hobbyists**
- **Profile**: Electronics enthusiasts, DIY hobbyists, students learning embedded systems
- **Current Behavior**: Building projects with Arduino/ESP32, following tutorials, sharing builds on platforms like Bilibili, Hackster.io
- **Pain Points**: 
  - Want to add voice AI to projects but lack expertise in ASR/LLM/TTS integration
  - Frustrated by expensive proprietary solutions ($100-300+)
  - Need customizable solutions for personal projects
  - Want to learn how modern AI voice systems work
- **Goals**: 
  - Build personalized AI devices (smart home controllers, voice assistants, interactive art)
  - Learn AI/embedded systems integration
  - Create unique projects to share with community
- **Technical Comfort**: Intermediate to advanced (comfortable with Arduino/ESP-IDF, basic C++)

**2. Hardware Developers & Product Makers**
- **Profile**: Small teams building IoT products, hardware startups, electronics manufacturers
- **Current Behavior**: Evaluating voice AI solutions, prototyping with development kits, considering proprietary platforms
- **Pain Points**:
  - Proprietary solutions create vendor lock-in and limit product differentiation
  - High licensing costs for commercial voice AI platforms
  - Need to support multiple hardware configurations
  - Require customization for specific use cases (custom wake words, branding, features)
- **Goals**:
  - Build commercial products with voice interfaces
  - Maintain control over firmware and user experience
  - Reduce development time and costs
  - Support multiple hardware SKUs with single codebase
- **Technical Comfort**: Advanced (embedded systems, firmware development, product engineering)

**3. Educational Institutions & Instructors**
- **Profile**: University professors, coding bootcamp instructors, makerspace coordinators
- **Current Behavior**: Teaching embedded systems, IoT, or AI courses using standard development boards
- **Pain Points**:
  - Need accessible, affordable platforms for teaching AI/embedded integration
  - Want students to understand complete systems, not just use black-box APIs
  - Limited budget for expensive development kits
  - Need projects that engage students with modern AI capabilities
- **Goals**:
  - Teach practical AI/embedded systems integration
  - Provide hands-on experience with real-world AI applications
  - Enable students to build portfolio projects
  - Demonstrate open-source development practices
- **Technical Comfort**: Advanced (teaching experience, curriculum development)

### Secondary Users

**4. Open-Source Contributors & Developers**
- **Profile**: Software developers contributing to open-source projects, ESP32 community members
- **Current Behavior**: Contributing to ESP-IDF projects, building ESP32 applications, maintaining open-source libraries
- **Pain Points**: 
  - Want to contribute to meaningful AI/embedded projects
  - Looking for well-architected codebases to learn from
  - Interested in extending platform capabilities
- **Goals**: 
  - Add new board support
  - Extend MCP server capabilities
  - Improve core features
  - Build community around project

**5. Privacy-Conscious Consumers**
- **Profile**: Users who want voice AI functionality but are concerned about data privacy
- **Current Behavior**: Avoiding commercial voice assistants, using local-only solutions
- **Pain Points**:
  - Commercial solutions send all audio to cloud
  - No control over data processing
  - Limited offline capabilities
- **Goals**:
  - Build voice assistant that processes data locally when possible
  - Maintain control over what data is sent to cloud
  - Customize device behavior

### User Journey

**Maker/Hobbyist Journey:**
1. Discovers project through GitHub, Bilibili tutorial, or maker community
2. Reviews supported hardware list, identifies compatible board they own or can purchase ($10-50)
3. Follows getting started guide to flash firmware (15-30 minutes)
4. Connects to xiaozhi.me server or sets up self-hosted server
5. Customizes assets (wake word, emojis, fonts) using web generator
6. Builds first project (smart home controller, voice assistant, interactive display)
7. Shares project with community, gets feedback
8. Extends functionality using MCP protocol for custom device control
9. Contributes board configuration or feature back to project

**Hardware Developer Journey:**
1. Evaluates voice AI solutions for product development
2. Discovers Xiaozhi ESP32, reviews architecture and MCP protocol
3. Tests on development hardware, evaluates latency and functionality
4. Integrates into product prototype, customizes for specific use case
5. Adds custom board configuration for product hardware
6. Develops product-specific MCP servers for device control
7. Launches product with Xiaozhi ESP32 firmware
8. Maintains fork or contributes improvements back to main project

---

## Success Metrics

### Business Objectives

As an open-source project, success is measured by community adoption, ecosystem growth, and real-world usage rather than revenue:

1. **Community Growth**: 
   - GitHub stars and forks (currently significant, target: continued organic growth)
   - Active contributors and maintainers
   - Community forum/discussion activity

2. **Hardware Ecosystem Expansion**:
   - Number of supported boards (currently 70+, target: 100+ by end of year)
   - Community-contributed board configurations
   - Third-party hardware manufacturers adopting platform

3. **Developer Adoption**:
   - Number of projects built on platform
   - Self-hosted server deployments
   - Commercial products using platform

4. **Platform Maturity**:
   - Code quality and architecture improvements
   - Documentation completeness
   - Stability and reliability metrics

### Key Performance Indicators

1. **GitHub Engagement**
   - Stars: Track growth rate (target: consistent monthly growth)
   - Forks: Measure developer interest (target: 10% fork-to-star ratio)
   - Issues/PRs: Community contribution activity (target: healthy issue resolution rate)

2. **Hardware Support**
   - New board configurations added per quarter (target: 5-10 new boards)
   - Board configuration quality (community feedback)
   - Hardware compatibility test coverage

3. **Documentation Quality**
   - Getting started guide completion rate (users successfully flashing firmware)
   - Documentation page views and engagement
   - Tutorial video views and engagement (Bilibili, YouTube)

4. **Server Ecosystem**
   - Number of self-hosted server deployments
   - Third-party server implementations (Python, Java, Go)
   - MCP server extensions created by community

5. **Commercial Adoption**
   - Number of commercial products using platform (tracked through community reports)
   - Commercial licensing inquiries (MIT allows commercial use)
   - Hardware manufacturer partnerships

---

## MVP Scope

### Core Features

**Current v2 Implementation (Already Delivered):**

1. **Core Voice Pipeline**
   - Offline voice wake-up (ESP-SR integration)
   - Streaming ASR processing
   - LLM integration (Qwen, DeepSeek support)
   - TTS synthesis
   - Voice activity detection
   - Speaker recognition (3D Speaker)

2. **Hardware Support**
   - Multi-platform support (ESP32-C3, ESP32-S3, ESP32-P4)
   - Board configuration system
   - Display support (OLED, LCD with emoji/expression system)
   - Audio codec support (OPUS)
   - Power management and battery display
   - GPIO control framework

3. **Communication Protocols**
   - WebSocket protocol implementation
   - MQTT + UDP hybrid protocol
   - OTA (Over-The-Air) update support
   - Network configuration (Wi-Fi, 4G ML307 Cat.1)

4. **MCP Integration**
   - Device-side MCP server (volume, LED, motor, GPIO control)
   - Cloud-side MCP client support
   - MCP protocol implementation

5. **Customization System**
   - Asset generation framework
   - Custom wake words
   - Custom fonts
   - Custom emojis/expressions
   - Custom chat backgrounds

6. **Multi-Language Support**
   - Chinese (Simplified)
   - English
   - Japanese
   - Extensible language framework

7. **Developer Experience**
   - ESP-IDF integration
   - Board configuration documentation
   - Custom board development guide
   - MCP protocol documentation

### Out of Scope for MVP

**Features Not Included (Future Considerations):**

1. **Advanced AI Features**
   - Multi-modal AI (vision, image processing) - planned for future
   - Local LLM execution on ESP32 - hardware limitations
   - Advanced emotion recognition - research phase

2. **Enterprise Features**
   - Multi-tenant server architecture
   - Advanced user management
   - Enterprise SSO integration
   - Compliance certifications (HIPAA, SOC2)

3. **Commercial Services**
   - Managed cloud hosting service
   - Commercial support contracts
   - Professional services/consulting

4. **Platform-Specific Features**
   - iOS/Android mobile apps (community projects exist)
   - Desktop applications (community projects exist)
   - Web-based management console (third-party implementations exist)

### MVP Success Criteria

The current v2 release has achieved MVP success through:

1. ✅ **Functional Completeness**: Core voice AI pipeline works end-to-end
2. ✅ **Hardware Diversity**: Supports 70+ different ESP32 boards
3. ✅ **Documentation**: Comprehensive getting started guides and developer documentation
4. ✅ **Community Adoption**: Active GitHub community with stars, forks, and contributions
5. ✅ **Real-World Usage**: Multiple commercial and hobbyist projects using platform
6. ✅ **Ecosystem Growth**: Third-party server implementations and client projects
7. ✅ **Stability**: v1 maintained until 2026, v2 actively developed

### Future Vision

**Short-Term (6-12 months):**
- Expand to 100+ supported boards
- Enhanced MCP server ecosystem
- Improved documentation and tutorials
- Performance optimizations
- Additional language support

**Medium-Term (1-2 years):**
- Multi-modal AI capabilities (vision, image processing)
- Enhanced local processing capabilities
- Advanced customization options
- Enterprise features (if community demand)
- Hardware manufacturer partnerships

**Long-Term Vision:**
- Become the de-facto open-source platform for ESP32-based voice AI
- Enable a thriving ecosystem of hardware, software, and services
- Drive innovation in edge AI and voice interfaces
- Support next-generation ESP32 chips and capabilities
- Bridge the gap between AI research and practical embedded applications

---

## Market Context

### Market Opportunity

The global voice assistant market is experiencing rapid growth, with embedded voice AI becoming increasingly important:

- **Smart Speaker Market**: $13.9B in 2023, projected to reach $35.5B by 2030 (CAGR 14.3%)
- **IoT Device Market**: 15.1B connected devices in 2023, growing to 29.4B by 2030
- **Edge AI Market**: $15.6B in 2023, projected to reach $107.5B by 2030 (CAGR 31.4%)

However, the **open-source embedded voice AI platform market** is relatively nascent, with Xiaozhi ESP32 positioned as an early leader in this space.

### Competitive Landscape

**Direct Competitors:**
- **ESP-SR + Custom Integration**: Developers building custom solutions using ESP-SR directly (more complex, less integrated)
- **Other ESP32 Voice Projects**: Fragmented community projects with limited hardware support and documentation

**Indirect Competitors:**
- **Commercial Voice AI Platforms**: Amazon Alexa Voice Service, Google Assistant SDK (vendor lock-in, licensing costs)
- **Raspberry Pi Voice Projects**: Higher power consumption, larger form factor, higher cost
- **Proprietary Development Kits**: Expensive, limited customization, closed-source

**Competitive Advantages:**
1. **Open Source**: Complete source code access vs. proprietary solutions
2. **Hardware Portability**: 70+ boards vs. single-board solutions
3. **MCP Architecture**: Extensible protocol vs. closed ecosystems
4. **Community**: Active GitHub community vs. vendor-controlled platforms
5. **Cost**: Free and open vs. $100-500+ development kits
6. **Customization**: Full control over assets and behavior vs. limited customization

### Market Positioning

Xiaozhi ESP32 occupies a unique position in the voice AI market:

- **Not competing with consumer voice assistants** (Alexa, Google) - targeting developers and makers instead
- **Complementary to commercial platforms** - provides open-source alternative for customization and control
- **Enabling new use cases** - supports applications commercial platforms don't address (custom hardware, privacy-sensitive deployments, educational projects)
- **Community-driven innovation** - faster feature development through open-source collaboration vs. vendor roadmaps

---

## Technical Preferences

### Technology Stack

**Firmware:**
- **Framework**: ESP-IDF (Espressif IoT Development Framework) v5.4+
- **Language**: C++ (Google C++ style guide)
- **Platforms**: ESP32-C3, ESP32-S3, ESP32-P4
- **Audio**: OPUS codec for efficient audio transmission
- **Voice Processing**: ESP-SR for offline wake word detection
- **Display**: LVGL for UI rendering, custom emote display system

**Communication:**
- **Protocols**: WebSocket, MQTT + UDP
- **Audio Streaming**: Real-time OPUS-encoded audio streams
- **OTA Updates**: ESP-IDF OTA framework

**AI Integration:**
- **ASR**: Cloud-based streaming ASR (Qwen, DeepSeek APIs)
- **LLM**: Cloud-based large language models (Qwen, DeepSeek)
- **TTS**: Cloud-based text-to-speech synthesis
- **Speaker Recognition**: 3D Speaker model integration

**Architecture:**
- **MCP Protocol**: Model Context Protocol for extensibility
- **Modular Design**: Board-specific configurations, plugin architecture
- **Asset System**: Custom asset generation and flashing

### Integration Requirements

- **Server Integration**: Compatible with xiaozhi.me official server or self-hosted implementations
- **Hardware Flexibility**: Must support wide range of ESP32 boards with varying capabilities
- **Network Options**: Wi-Fi and 4G (ML307 Cat.1) support
- **Display Flexibility**: OLED and LCD displays with different resolutions and controllers

### Performance Requirements

- **Wake Word Latency**: < 500ms from utterance to detection
- **Voice Processing Latency**: < 2s end-to-end (wake → response)
- **Power Consumption**: Optimized for battery-powered applications
- **Memory Efficiency**: Must run on ESP32 with limited RAM/Flash

---

## Organizational Context

### Project Structure

**Open-Source Community Project:**
- **License**: MIT (allows commercial use, modification, distribution)
- **Maintainer**: Robert (虾哥) and community contributors
- **Repository**: GitHub (78/xiaozhi-esp32)
- **Community**: QQ群 1011329060, GitHub Issues/PRs, Bilibili tutorials

### Strategic Alignment

**Mission**: Enable anyone to build intelligent voice interaction devices using open-source software and accessible hardware.

**Values:**
- **Openness**: Complete source code access, MIT licensing
- **Accessibility**: Support for affordable hardware ($5-50 range)
- **Community**: Community-driven development and support
- **Education**: Comprehensive documentation and tutorials
- **Innovation**: MCP protocol and extensible architecture

### Stakeholder Considerations

**Primary Stakeholders:**
- **Project Maintainer**: Robert (ensures project direction, code quality, community health)
- **Contributors**: Developers adding features, board support, documentation
- **Users**: Makers, developers, educators using the platform
- **Hardware Manufacturers**: Companies building ESP32-based products

**Community Governance:**
- Open development process through GitHub
- Community feedback through Issues and Discussions
- Contribution guidelines and code review process
- Version management (v1 stable branch, v2 active development)

---

## Risks and Assumptions

### Key Risks

1. **Maintainer Burnout**
   - **Risk**: Single primary maintainer could lead to project stagnation
   - **Mitigation**: Active community contribution, clear contribution guidelines, potential maintainer transition plan

2. **Hardware Fragmentation**
   - **Risk**: Supporting 70+ boards could lead to maintenance burden
   - **Mitigation**: Modular board configuration system, community-maintained board configs, clear hardware requirements

3. **Technology Dependencies**
   - **Risk**: Dependence on ESP-IDF, cloud AI services, MCP protocol evolution
   - **Mitigation**: Track upstream dependencies, maintain compatibility layers, contribute to dependency projects

4. **Market Competition**
   - **Risk**: Large tech companies could release competing open-source solutions
   - **Mitigation**: First-mover advantage, community lock-in, focus on unique MCP architecture

5. **Documentation Debt**
   - **Risk**: Rapid feature development could outpace documentation
   - **Mitigation**: Documentation as part of contribution process, community documentation efforts, video tutorials

### Critical Assumptions

1. **ESP32 Platform Longevity**: Assumes Espressif continues developing ESP32 platform and ESP-IDF framework
2. **Cloud AI Service Availability**: Assumes Qwen, DeepSeek, and other AI services remain accessible and affordable
3. **Community Engagement**: Assumes continued community interest and contribution
4. **MCP Protocol Adoption**: Assumes MCP protocol gains traction in AI/embedded communities
5. **Hardware Availability**: Assumes ESP32 boards remain widely available and affordable

### Open Questions

1. **Commercial Adoption**: How many commercial products will adopt the platform? What support do they need?
2. **Enterprise Features**: Is there demand for enterprise-focused features (multi-tenancy, SSO, compliance)?
3. **Local AI**: Can local LLM execution on ESP32 become feasible with future hardware?
4. **Multi-Modal AI**: Should the platform expand to support vision/image processing capabilities?
5. **Hardware Partnerships**: Should the project partner with hardware manufacturers for official support?

---

## Timeline

### Current Status (v2 Active Development)

- **v1 Stable**: Maintained until February 2026
- **v2 Development**: Active development with regular releases
- **Community Growth**: Ongoing through GitHub, tutorials, community engagement

### Future Milestones

**Near-Term (3-6 months):**
- Expand board support to 100+ boards
- Enhance MCP server ecosystem
- Improve documentation and tutorials
- Performance optimizations

**Medium-Term (6-12 months):**
- Multi-modal AI capabilities exploration
- Enhanced customization options
- Additional language support
- Community-driven feature development

**Long-Term (1-2 years):**
- Next-generation ESP32 chip support
- Advanced local AI capabilities
- Hardware manufacturer partnerships
- Enterprise feature evaluation (if demand exists)

---

## Supporting Materials

### Project Resources

- **GitHub Repository**: https://github.com/78/xiaozhi-esp32
- **Documentation**: Comprehensive docs in repository
- **Tutorials**: Bilibili video tutorials (Chinese)
- **Community**: QQ群 1011329060
- **Official Server**: https://xiaozhi.me

### Related Projects

- **Server Implementations**: 
  - Python: xinnan-tech/xiaozhi-esp32-server
  - Java: joey-zhou/xiaozhi-esp32-server-java
  - Go: AnimeAIChat/xiaozhi-server-go

- **Client Projects**:
  - Python: huangjunsen0406/py-xiaozhi
  - Android: TOM88812/xiaozhi-android-client
  - Linux: 100askTeam/xiaozhi-linux

- **Hardware Projects**:
  - 70+ supported ESP32 development boards
  - Custom board configurations in main/boards/

### Technical Documentation

- Custom Board Development Guide: docs/custom-board.md
- MCP Protocol Documentation: docs/mcp-protocol.md
- MCP Usage Guide: docs/mcp-usage.md
- WebSocket Protocol: docs/websocket.md
- MQTT+UDP Protocol: docs/mqtt-udp.md

---

_This Product Brief captures the vision and requirements for xiaozhi-esp32._

_It was created through collaborative discovery and reflects the unique needs of this open-source hardware/software project._

_Next: Use the PRD workflow to create detailed product requirements from this brief, or proceed directly to technical specification and architecture planning._

