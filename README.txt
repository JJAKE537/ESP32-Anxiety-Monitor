1.文件夹说明
	工程编译依赖ESP-IDF组件管理器下载生成的managed_components目录，本次提交保留该目录中的官方/开源依赖源码；
	managed_components主要包括LVGL图形界面库、EK79007 LCD显示驱动、ESP LCD Touch及GT911触摸驱动、ESP-Hosted与WI-FI Remote通信组件，
	以及cmake_utilities、eppp_link、esp_serial_slave_link等配套构建和链路支持组件。



2.编译环境说明
	本工程基于ESP-IDF v5.5.4开发，主要使用VS Code+ESP-IDF插件完成工程配置、编译和烧录；
	工程目标芯片配置为ESP32-P4，相关目标芯片、Flash、分区表、FreeRTOS、外设驱动等配置已保存在sdkconfig和sdkconfig.defaults中；
	
	本提交包为完整ESP-IDF工程源码包，不包含ESP-IDF框架本体和编译工具链；
	编译前需在电脑中安装并配置ESP-IDF v5.5.4及ESP32-P4对应工具链；
	在ESP-IDF环境激活后，可进入本工程根目录执行idf.py build完成编译；
	建议将工程解压到英文路径下编译。
	
3.文件架构
	嵌入式端重点代码【核心业务代码】主要包括main主程序代码和components自定义组件代码。
	系统主要实现柔性电阻脉搏波数据采集、LCD终端交互界面显示、PRV特征参数计算与显示、焦虑指数显示及AI分析建议显示等功能。
components/								  自定义功能组件目录
main/                                     系统主程序目录
managed_components/  					  ESP-IDF组件管理器生成的官方/开源依赖组件源码
CMakeLists.txt                            ESP-IDF工程入口与项目名称配置
dependencies.lock                         组件依赖版本锁定文件
idf_component.yml                         工程级组件依赖声明
partitions.csv                            Flash分区表配置
README.txt                                工程与文件结构说明
sdkconfig                                 当前ESP-IDF工程配置
sdkconfig.defaults                        工程默认配置



4.项目重点代码/
|---main/
|	|---app_FreeRtos.c					  系统入口及采集、处理、特征和焦虑计算任务FreeRTOS调度
|	|---app_ai_client.c                   连接AI服务并上传数据、接收分析结果
|	|---app_ai_client.h                   AI客户端对外接口声明
|	|---CMakeLists.txt                    main组件源码及依赖配置
|	|---idf_component.yml                 main组件外部依赖声明
|
|
|---components/
|	|---app_common/
|		|---include/
|			|---app_config.h		      硬件参数、任务参数和算法参数配置
|			|---app_message.h             任务间消息及数据结构定义
|		|---app_rtc.c                     Wi-Fi、SNTP校时及时间状态管理
|		|---CMakeLists.txt                app_common组件构建配置
|	|---ppi_process/
|		|---include/
|			|---ppi_process.h             PPI处理组件接口声明
|		|---ppi_process.c                 脉搏波滤波、峰值检测及PPI序列提取
|		|---ppi_time_feat.c               计算PR、RMSSD、SD2等时域特征
|		|---ppi_fre_feat.c                计算LF、HF、LF/HF等频域特征
|		|---ppi_nl_feat.c                 计算WE、DFA、VAI等非线性特征
|		|---anxiety_idx_cal.c             基于九项特征计算焦虑指数
|		|---CMakeLists.txt                ppi_process组件构建配置
|	|---app_ui/
|		|---fonts/
|			|---ai_chinese.ttf			  AI中文分析文本使用的字体文件
|	    |---include/ 
|			|---app_lvgl.h                UI组件公共接口与数据类型声明
|		|---app_lvgl.c                    LCD、LVGL主界面及数据显示管理
|		|---app_touch_gt911.c             GT911触摸控制器初始化与输入处理
|		|---app_ui_history.c              历史记录查询、显示及导出交互
|		|---app_ui_statics.c              数据统计、趋势和特征汇总显示
|		|---app_ui_config.c               阈值、亮度、主题等系统设置界面
|		|---app_ui_about.c                设备信息及关于页面
|		|---app_ui_font_16.c              LVGL16号中文字库数据
|		|---CMakeLists.txt                app_ui组件构建及字体嵌入配置
