using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using Windows.Foundation;
using Windows.Globalization;
using Windows.Graphics.Imaging;
using Windows.Media.Ocr;
using Windows.Storage;

internal static class Program
{
    private static T Await<T>(IAsyncOperation<T> operation)
    {
        Task<T> task = operation.AsTask();
        if (!task.Wait(15000))
        {
            operation.Cancel();
            throw new TimeoutException("OCR operation timed out");
        }
        return task.GetAwaiter().GetResult();
    }

    private static int Main(string[] args)
    {
        Console.OutputEncoding = new UTF8Encoding(false);
        try
        {
            if (args.Length != 1 || !IsAbsolutePath(args[0]) ||
                !string.Equals(Path.GetExtension(args[0]), ".png", StringComparison.OrdinalIgnoreCase))
                throw new ArgumentException("Usage: ocr.exe <absolute PNG path>");

            OcrEngine engine = OcrEngine.TryCreateFromLanguage(new Language("zh-Hans"));
            if (engine == null)
                throw new InvalidOperationException("Windows Chinese OCR language is not installed");

            StorageFile file = Await(StorageFile.GetFileFromPathAsync(Path.GetFullPath(args[0])));
            using (var stream = Await(file.OpenAsync(FileAccessMode.Read)))
            {
                BitmapDecoder decoder = Await(BitmapDecoder.CreateAsync(stream));
                using (SoftwareBitmap bitmap = Await(decoder.GetSoftwareBitmapAsync()))
                {
                    OcrResult result = Await(engine.RecognizeAsync(bitmap));
                    var lines = new List<object>();
                    foreach (OcrLine line in result.Lines)
                    {
                        var words = new List<object>();
                        foreach (OcrWord word in line.Words)
                        {
                            Rect rect = word.BoundingRect;
                            words.Add(new { text = word.Text, x = rect.X, y = rect.Y,
                                width = rect.Width, height = rect.Height });
                        }
                        lines.Add(new { text = line.Text, words = words });
                    }
                    var serializer = new JavaScriptSerializer { MaxJsonLength = int.MaxValue };
                    Console.WriteLine(serializer.Serialize(new { text = result.Text, lines = lines }));
                }
            }
            return 0;
        }
        catch (Exception exception)
        {
            Console.Error.WriteLine(exception.GetBaseException().Message);
            return 1;
        }
    }

    private static bool IsAbsolutePath(string path)
    {
        // Framework Path.IsPathRooted also accepts drive-relative and root-relative paths.
        return !string.IsNullOrEmpty(path) &&
            ((path.Length >= 3 && char.IsLetter(path[0]) && path[1] == ':' &&
                (path[2] == '\\' || path[2] == '/')) || path.StartsWith(@"\\"));
    }
}
