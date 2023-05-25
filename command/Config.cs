using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using Newtonsoft.Json;
using System.Windows.Forms;
using System.IO;

namespace command
{
    public class Config
    {
        public class IpPort_Config
        {
            public string Ip = "127.0.0.1";
            public int Port = 8080;
        }
        public class Config_Config
        {
            public IpPort_Config Harbor = new IpPort_Config();
        }
        private Config_Config m_Config;
        private string m_Path;
        public Config()
        {
            m_Path = Application.StartupPath + "\\command\\config.json";
            if (!File.Exists(m_Path))
            {
                m_Config = new Config_Config();
            }
            else
            {
                try
                {
                    m_Config = JsonConvert.DeserializeObject<Config_Config>(File.ReadAllText(m_Path));
                }
                catch
                {
                    m_Config = new Config_Config();
                }
            }
        }
        public Config_Config Configs
        {
            get { return m_Config; }
        }
        public void Save()
        {
            try
            {
                using (StreamWriter sw = new StreamWriter(File.Open(m_Path, FileMode.Create)))
                {
                    string jsonString = JsonConvert.SerializeObject(m_Config, Formatting.Indented);
                    sw.Write(jsonString);
                    sw.Flush();
                }
            }
            catch
            { }
        }
    }
}
