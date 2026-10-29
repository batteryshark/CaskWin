// Actual CLR synchronization and named-section regression, .NET Framework 4.
using System;
using System.Diagnostics;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Threading;
class NamedObjectsManaged {
    static int failures;
    static string Name(string stem,string suffix) { return "Local\\"+stem+suffix; }
    static void Check(bool ok,string message) { Console.WriteLine((ok?"PASS ":"FAIL ")+message);if(!ok)failures++; }
    static int Main(string[] args) {
        try {
            if(args.Length<3)return 2;string mode=args[0],stem=args[1];int value=int.Parse(args[2]);
            if(mode=="child") {
                using(var mutex=Mutex.OpenExisting(Name(stem,".mutex"))) { Check(mutex.WaitOne(1000),"CLR child opens same-prefix mutex");mutex.ReleaseMutex(); }
                using(var signal=EventWaitHandle.OpenExisting(Name(stem,".event")))signal.Set();
                using(var semaphore=Semaphore.OpenExisting(Name(stem,".semaphore")))Check(semaphore.WaitOne(1000),"CLR child consumes same-prefix semaphore");
                using(var memory=MemoryMappedFile.OpenExisting(Name(stem,".mapping")))using(var view=memory.CreateViewAccessor()) {
                    Check(view.ReadInt32(0)==value,"CLR child sees prefix-specific section");view.Write(0,value+1);
                }
                return failures==0?0:1;
            }
            if(args.Length<5)return 2;string control=args[3],child=args[4];
            try { using(var unexpected=Mutex.OpenExisting(Name(stem,".mutex")))Check(false,"CLR host mutex must be hidden"); }
            catch(WaitHandleCannotBeOpenedException) { Check(true,"CLR host mutex cannot satisfy private open"); }
            try { using(var unexpected=EventWaitHandle.OpenExisting(Name(stem,".event")))Check(false,"CLR host event must be hidden"); }
            catch(WaitHandleCannotBeOpenedException) { Check(true,"CLR host event cannot satisfy private open"); }
            bool mutexNew,eventNew,semNew;
            using(var mutex=new Mutex(false,Name(stem,".mutex"),out mutexNew))
            using(var signal=new EventWaitHandle(false,EventResetMode.ManualReset,Name(stem,".event"),out eventNew))
            using(var semaphore=new Semaphore(1,4,Name(stem,".semaphore"),out semNew))
            using(var memory=MemoryMappedFile.CreateNew(Name(stem,".mapping"),4096))
            using(var view=memory.CreateViewAccessor()) {
                Check(mutexNew&&eventNew&&semNew,"CLR objects are fresh within this prefix");view.Write(0,value);
                var start=new ProcessStartInfo(child,"child "+stem+" "+value);start.UseShellExecute=false;
                start.EnvironmentVariables.Clear();start.EnvironmentVariables["SystemRoot"]=Environment.GetEnvironmentVariable("SystemRoot");
                using(var process=Process.Start(start)) { Check(process.WaitForExit(15000)&&process.ExitCode==0,"opposite-bitness CLR child with custom environment succeeds"); }
                Check(view.ReadInt32(0)==value+1&&signal.WaitOne(0),"CLR child shares prefix section and event");signal.Reset();
                Check(!semaphore.WaitOne(0),"CLR child consumed only private semaphore");File.WriteAllText(control+".ready","ready");
                var timer=Stopwatch.StartNew();while(!File.Exists(control+".stop")) { if(timer.ElapsedMilliseconds>60000)throw new TimeoutException();Thread.Sleep(20); }
                Check(view.ReadInt32(0)==value+1&&!signal.WaitOne(0),"CLR sibling prefix and host remain separate");
            }
            Console.WriteLine("Managed named objects "+(IntPtr.Size*8)+"-bit failures="+failures);return failures==0?0:1;
        } catch(Exception error) { Console.WriteLine("FAIL "+error);return 1; }
    }
}
