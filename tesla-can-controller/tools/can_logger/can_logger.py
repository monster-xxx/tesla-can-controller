#!/usr/bin/env python3
"""
Tesla CAN Logger Tool
用于记录和分析Tesla Model Y的CAN总线数据
"""

import can
import cantools
import csv
import json
import time
import argparse
from datetime import datetime
from pathlib import Path
import signal
import sys

class TeslaCANLogger:
    def __init__(self, interface='vcan0', bitrate=500000, dbc_file=None):
        """
        初始化CAN记录器
        
        Args:
            interface: CAN接口名称 (vcan0, can0, slcan0等)
            bitrate: CAN波特率 (默认500kbps)
            dbc_file: DBC文件路径
        """
        self.interface = interface
        self.bitrate = bitrate
        self.running = False
        
        # 加载DBC数据库
        if dbc_file and Path(dbc_file).exists():
            print(f"加载DBC文件: {dbc_file}")
            self.db = cantools.database.load_file(dbc_file)
            self.use_dbc = True
        else:
            print("未提供DBC文件，使用原始CAN数据")
            self.db = None
            self.use_dbc = False
        
        # 初始化CAN总线
        self.bus = None
        self.setup_can_bus()
        
        # 数据存储
        self.data = []
        self.message_count = 0
        self.start_time = None
        
    def setup_can_bus(self):
        """设置CAN总线连接"""
        try:
            self.bus = can.interface.Bus(
                channel=self.interface,
                bustype='socketcan',
                bitrate=self.bitrate
            )
            print(f"成功连接到CAN接口: {self.interface} @ {self.bitrate}bps")
        except Exception as e:
            print(f"连接CAN接口失败: {e}")
            print("尝试创建虚拟CAN接口...")
            self.create_virtual_can()
    
    def create_virtual_can(self):
        """创建虚拟CAN接口"""
        try:
            import subprocess
            subprocess.run(['sudo', 'modprobe', 'vcan'], check=True)
            subprocess.run(['sudo', 'ip', 'link', 'add', 'dev', 'vcan0', 'type', 'vcan'], check=True)
            subprocess.run(['sudo', 'ip', 'link', 'set', 'up', 'vcan0'], check=True)
            print("虚拟CAN接口 vcan0 创建成功")
            
            # 重新连接
            self.bus = can.interface.Bus(
                channel='vcan0',
                bustype='socketcan',
                bitrate=self.bitrate
            )
        except Exception as e:
            print(f"创建虚拟CAN接口失败: {e}")
            sys.exit(1)
    
    def parse_message(self, msg):
        """解析CAN消息"""
        parsed = {
            'timestamp': msg.timestamp,
            'can_id': hex(msg.arbitration_id),
            'can_id_dec': msg.arbitration_id,
            'data': msg.data.hex(),
            'data_length': msg.dlc,
            'is_extended': msg.is_extended_id,
            'is_remote': msg.is_remote_frame,
            'is_error': msg.is_error_frame
        }
        
        # 使用DBC解析
        if self.use_dbc and self.db:
            try:
                decoded = self.db.decode_message(msg.arbitration_id, msg.data)
                parsed['decoded'] = decoded
                
                # 提取关键信号
                if msg.arbitration_id == 0x1A6:  # 车速
                    if 'VehicleSpeed' in decoded:
                        parsed['vehicle_speed'] = decoded['VehicleSpeed']
                elif msg.arbitration_id == 0x352:  # 电池
                    if 'SOC' in decoded:
                        parsed['battery_soc'] = decoded['SOC']
                elif msg.arbitration_id == 0x108:  # 挡位
                    if 'GearPosition' in decoded:
                        parsed['gear_position'] = decoded['GearPosition']
                        
            except Exception as e:
                parsed['decode_error'] = str(e)
        
        return parsed
    
    def log_message(self, msg):
        """记录CAN消息"""
        parsed = self.parse_message(msg)
        self.data.append(parsed)
        self.message_count += 1
        
        # 实时显示
        if self.message_count % 100 == 0:
            self.print_status()
        
        # 详细显示特定消息
        if parsed['can_id_dec'] in [0x1A6, 0x352, 0x108]:
            self.print_important_message(parsed)
    
    def print_status(self):
        """打印状态信息"""
        elapsed = time.time() - self.start_time
        rate = self.message_count / elapsed if elapsed > 0 else 0
        
        print(f"\r消息数: {self.message_count} | "
              f"速率: {rate:.1f} msg/s | "
              f"运行时间: {elapsed:.1f}s", end='')
    
    def print_important_message(self, parsed):
        """打印重要消息"""
        print(f"\n{'='*60}")
        print(f"CAN ID: {parsed['can_id']}")
        print(f"时间戳: {parsed['timestamp']:.6f}")
        print(f"数据: {parsed['data']}")
        
        if 'decoded' in parsed:
            print("解析结果:")
            for key, value in parsed['decoded'].items():
                print(f"  {key}: {value}")
        
        if 'vehicle_speed' in parsed:
            print(f"车速: {parsed['vehicle_speed']} km/h")
        if 'battery_soc' in parsed:
            print(f"电池电量: {parsed['battery_soc']} %")
        if 'gear_position' in parsed:
            gear_names = ['P', 'R', 'N', 'D', 'S', 'L', 'M', 'Invalid']
            gear_idx = int(parsed['gear_position'])
            if 0 <= gear_idx < len(gear_names):
                print(f"挡位: {gear_names[gear_idx]}")
    
    def save_data(self, format='json', filename=None):
        """保存记录的数据"""
        if not filename:
            timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
            filename = f"tesla_can_log_{timestamp}"
        
        if format == 'json':
            filepath = f"{filename}.json"
            with open(filepath, 'w') as f:
                json.dump(self.data, f, indent=2, default=str)
            print(f"\n数据已保存到: {filepath}")
            
        elif format == 'csv':
            filepath = f"{filename}.csv"
            if self.data:
                # 提取所有可能的字段
                fieldnames = set()
                for entry in self.data:
                    fieldnames.update(entry.keys())
                
                with open(filepath, 'w', newline='') as f:
                    writer = csv.DictWriter(f, fieldnames=sorted(fieldnames))
                    writer.writeheader()
                    writer.writerows(self.data)
                print(f"\n数据已保存到: {filepath}")
        
        elif format == 'raw':
            filepath = f"{filename}.log"
            with open(filepath, 'w') as f:
                for entry in self.data:
                    line = f"{entry['timestamp']} {entry['can_id']} {entry['data']}\n"
                    f.write(line)
            print(f"\n原始数据已保存到: {filepath}")
    
    def run(self, duration=None, max_messages=None):
        """运行CAN记录器"""
        print(f"\n开始记录CAN数据...")
        print(f"接口: {self.interface}")
        print(f"波特率: {self.bitrate}")
        print(f"使用DBC: {self.use_dbc}")
        print("按Ctrl+C停止记录\n")
        
        self.start_time = time.time()
        self.running = True
        
        message_count = 0
        
        try:
            while self.running:
                # 检查停止条件
                if duration and (time.time() - self.start_time) > duration:
                    print(f"\n达到运行时间限制: {duration}秒")
                    break
                
                if max_messages and message_count >= max_messages:
                    print(f"\n达到消息数量限制: {max_messages}")
                    break
                
                # 接收消息
                msg = self.bus.recv(timeout=0.1)
                if msg:
                    self.log_message(msg)
                    message_count += 1
                    
        except KeyboardInterrupt:
            print("\n\n用户中断记录")
        finally:
            self.stop()
    
    def stop(self):
        """停止记录器"""
        self.running = False
        if self.bus:
            self.bus.shutdown()
        
        # 打印最终统计
        elapsed = time.time() - self.start_time
        print(f"\n{'='*60}")
        print(f"记录完成!")
        print(f"总消息数: {self.message_count}")
        print(f"总运行时间: {elapsed:.2f}秒")
        print(f"平均速率: {self.message_count/elapsed:.1f} 消息/秒")
        
        # 分析数据
        self.analyze_data()
    
    def analyze_data(self):
        """分析记录的数据"""
        if not self.data:
            print("没有数据可分析")
            return
        
        print(f"\n数据分析:")
        print(f"{'='*60}")
        
        # 统计CAN ID频率
        id_counts = {}
        for entry in self.data:
            can_id = entry['can_id']
            id_counts[can_id] = id_counts.get(can_id, 0) + 1
        
        print(f"唯一的CAN ID数量: {len(id_counts)}")
        print(f"\n最常见的CAN ID:")
        sorted_ids = sorted(id_counts.items(), key=lambda x: x[1], reverse=True)
        for can_id, count in sorted_ids[:10]:
            percentage = (count / len(self.data)) * 100
            print(f"  {can_id}: {count} 消息 ({percentage:.1f}%)")
        
        # 提取车辆状态
        vehicle_speeds = []
        battery_socs = []
        gear_positions = []
        
        for entry in self.data:
            if 'vehicle_speed' in entry:
                vehicle_speeds.append(entry['vehicle_speed'])
            if 'battery_soc' in entry:
                battery_socs.append(entry['battery_soc'])
            if 'gear_position' in entry:
                gear_positions.append(entry['gear_position'])
        
        if vehicle_speeds:
            print(f"\n车速统计:")
            print(f"  样本数: {len(vehicle_speeds)}")
            print(f"  最小值: {min(vehicle_speeds):.1f} km/h")
            print(f"  最大值: {max(vehicle_speeds):.1f} km/h")
            print(f"  平均值: {sum(vehicle_speeds)/len(vehicle_speeds):.1f} km/h")
        
        if battery_socs:
            print(f"\n电池电量统计:")
            print(f"  样本数: {len(battery_socs)}")
            print(f"  最小值: {min(battery_socs):.1f} %")
            print(f"  最大值: {max(battery_socs):.1f} %")
            print(f"  平均值: {sum(battery_socs)/len(battery_socs):.1f} %")
        
        if gear_positions:
            gear_names = ['P', 'R', 'N', 'D', 'S', 'L', 'M', 'Invalid']
            gear_counts = {}
            for pos in gear_positions:
                idx = int(pos)
                if 0 <= idx < len(gear_names):
                    gear_name = gear_names[idx]
                    gear_counts[gear_name] = gear_counts.get(gear_name, 0) + 1
            
            print(f"\n挡位分布:")
            for gear, count in gear_counts.items():
                percentage = (count / len(gear_positions)) * 100
                print(f"  {gear}: {count} 次 ({percentage:.1f}%)")

def signal_handler(sig, frame):
    """处理信号中断"""
    print("\n接收到中断信号，正在停止...")
    sys.exit(0)

def main():
    """主函数"""
    parser = argparse.ArgumentParser(description='Tesla CAN总线数据记录器')
    parser.add_argument('-i', '--interface', default='vcan0',
                       help='CAN接口名称 (默认: vcan0)')
    parser.add_argument('-b', '--bitrate', type=int, default=500000,
                       help='CAN波特率 (默认: 500000)')
    parser.add_argument('-d', '--dbc', help='DBC文件路径')
    parser.add_argument('-t', '--duration', type=float,
                       help='记录持续时间(秒)')
    parser.add_argument('-m', '--max-messages', type=int,
                       help='最大消息数量')
    parser.add_argument('-o', '--output', help='输出文件名前缀')
    parser.add_argument('-f', '--format', choices=['json', 'csv', 'raw'],
                       default='json', help='输出格式 (默认: json)')
    parser.add_argument('--analyze', action='store_true',
                       help='仅分析现有数据文件')
    parser.add_argument('--file', help='要分析的数据文件')
    
    args = parser.parse_args()
    
    # 注册信号处理器
    signal.signal(signal.SIGINT, signal_handler)
    
    if args.analyze:
        # 分析模式
        if not args.file:
            print("错误: 分析模式需要指定文件")
            parser.print_help()
            return
        
        print(f"分析文件: {args.file}")
        with open(args.file, 'r') as f:
            if args.file.endswith('.json'):
                data = json.load(f)
            else:
                print("仅支持JSON格式文件分析")
                return
        
        # 创建记录器实例用于分析
        logger = TeslaCANLogger()
        logger.data = data
        logger.message_count = len(data)
        logger.analyze_data()
        
    else:
        # 记录模式
        logger = TeslaCANLogger(
            interface=args.interface,
            bitrate=args.bitrate,
            dbc_file=args.dbc
        )
        
        # 开始记录
        logger.run(
            duration=args.duration,
            max_messages=args.max_messages
        )
        
        # 保存数据
        if logger.data:
            logger.save_data(format=args.format, filename=args.output)

if __name__ == '__main__':
    main()