import torch
import torch.nn as nn
import torch.nn.functional as F
import numpy as np

class moving_avg(nn.Module):
    """
    Moving average block to highlight the trend of time series
    移动平均模块：通过平均池化计算时间序列的趋势成分。使用镜像填充处理边界值，保持序列长度不变。
    """
    def __init__(self, kernel_size, stride):
        super(moving_avg, self).__init__()
        self.kernel_size = kernel_size  # 移动平均的窗口大小
        # 1D平均池化层，用于计算移动平均值
        self.avg = nn.AvgPool1d(kernel_size=kernel_size, stride=stride, padding=0)

    def forward(self, x):
        # 在时间序列两端进行填充（镜像填充）
        front = x[:, 0:1, :].repeat(1, (self.kernel_size - 1) // 2, 1)  # 复制序列开头值
        end = x[:, -1:, :].repeat(1, (self.kernel_size - 1) // 2, 1)  # 复制序列结尾值
        x = torch.cat([front, x, end], dim=1)  # 拼接成扩展后的序列


        x = self.avg(x.permute(0, 2, 1))
        x = x.permute(0, 2, 1)
        return x


class series_decomp(nn.Module):
    """
    Series decomposition block
    """
    def __init__(self, kernel_size):
        super(series_decomp, self).__init__()
        self.moving_avg = moving_avg(kernel_size, stride=1)

    def forward(self, x):
        # 滑动平均
        moving_mean = self.moving_avg(x)
        # 季节趋势性
        res = x - moving_mean
        return res, moving_mean

class Model(nn.Module):
    """
    Decomposition-Linear
    """
    def __init__(self, configs):
        super(Model, self).__init__()
        # 从配置中获取参数
        self.seq_len = configs.seq_len  # 输入序列长度
        self.pred_len = configs.pred_len   # 预测序列长度

        # Decompsition Kernel Size
        kernel_size = 25   # 移动平均窗口大小
        self.decompsition = series_decomp(kernel_size)   # 序列分解模块
        self.individual = configs.individual  # 是否为每个通道使用独立模型
        self.channels = configs.enc_in  # 输入通道数（特征维度）

        # 根据individual标志选择模型结构
        if self.individual:
            self.Linear_Seasonal = nn.ModuleList()  # 季节性分量线性层列表
            self.Linear_Trend = nn.ModuleList()   # 趋势分量线性层列表
            self.Linear_Decoder = nn.ModuleList()  # 解码器层列表（未使用）
            
            for i in range(self.channels):
                # 为每个通道创建季节性线性层
                self.Linear_Seasonal.append(nn.Linear(self.seq_len,self.pred_len))
                # 为每个通道创建趋势线性层
                self.Linear_Trend.append(nn.Linear(self.seq_len,self.pred_len))

                # 初始化权重为1/seq_len
                self.Linear_Seasonal[i].weight = nn.Parameter((1/self.seq_len)*torch.ones([self.pred_len,self.seq_len]))
                self.Linear_Trend[i].weight = nn.Parameter((1/self.seq_len)*torch.ones([self.pred_len,self.seq_len]))
                # 为每个通道创建解码器（实际未使用）
                self.Linear_Decoder.append(nn.Linear(self.seq_len, self.pred_len))
        else:# 所有通道共享模型
            # 季节性分量共享线性层
            self.Linear_Seasonal = nn.Linear(self.seq_len,self.pred_len)
            # 趋势分量共享线性层
            self.Linear_Trend = nn.Linear(self.seq_len,self.pred_len)
            # 解码器层（未使用）
            self.Linear_Decoder = nn.Linear(self.seq_len, self.pred_len)

            # 初始化共享权重为1/seq_len
            self.Linear_Seasonal.weight = nn.Parameter((1/self.seq_len)*torch.ones([self.pred_len,self.seq_len]))
            self.Linear_Trend.weight = nn.Parameter((1/self.seq_len)*torch.ones([self.pred_len,self.seq_len]))

    def forward(self, x):
        # x: [Batch, Input length, Channel]
        # 季节与时间趋势性分解
        seasonal_init, trend_init = self.decompsition(x)
        # 将维度索引2与维度索引1交换
        seasonal_init, trend_init = seasonal_init.permute(0,2,1), trend_init.permute(0,2,1)
        if self.individual:
            seasonal_output = torch.zeros([seasonal_init.size(0),seasonal_init.size(1),self.pred_len],dtype=seasonal_init.dtype).to(seasonal_init.device)
            trend_output = torch.zeros([trend_init.size(0),trend_init.size(1),self.pred_len],dtype=trend_init.dtype).to(trend_init.device)
            for i in range(self.channels):
                # 使用全连接层得到季节性
                seasonal_output[:,i,:] = self.Linear_Seasonal[i](seasonal_init[:,i,:])
                # 使用全连接层得到趋势性
                trend_output[:,i,:] = self.Linear_Trend[i](trend_init[:,i,:])
                # 两者共享所有权重
        else:
            seasonal_output = self.Linear_Seasonal(seasonal_init)
            trend_output = self.Linear_Trend(trend_init)
        # 将季节性与趋势性相加
        x = seasonal_output + trend_output
        # 交换维度位置
        return x.permute(0,2,1) # to [Batch, Output length, Channel]


