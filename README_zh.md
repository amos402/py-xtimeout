## 检查函数运行是否超时并且可以跟踪和处理它

## 功能
* 检查函数运行是否超时
* 超时回调和原函数在同一线程上
* 支持多线程, 支持嵌套使用

## 使用
```python
def on_timeout(start_time: float):
    """
    :param start_time: 函数调用开始时间
    """
    traceback.print_stack()
    pdb.set_trace()
    raise Exception("time_out")

# 时间单位是毫秒
@pymonitor.check_time(10, on_timeout)
def function_1():
    pass

def function_2():
    with pymonitor.check_context(20, on_timeout):
        # do something
        with pymonitor.check_context(10, on_timeout):
            # do something
```

## 对比其它实现方式
下面是和一些其它实现方式的大致对比

* 使用`signal`模块然后发送信号
    - 只能支持检测主线程
    - 不能很好支持嵌套, 因为一个信号只能一个回调. 如果要嵌套你需要不断地进入回调并重新`alarm`, 开销根据检测精度而定
    - 仅支持Linux

* 新开工作线程做处理然后本线程等待超时后处理(例如结束它)
    - 无法切入到超时的函数进行操作
    - 来自于线程的开销

* 使用`sys.settrace`持续跟踪
    - 消耗巨大
