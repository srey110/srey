using command.DB;
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Data;
using System.Drawing;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using System.Windows.Forms;

namespace command
{
    public partial class FmTasks : Form
    {
        private int m_curId = -1;
        private string m_strTitle;
        private List<SQLite_Tasks> m_lstTasks;
        public FmTasks(List<SQLite_Tasks> lstTasks)
        {
            InitializeComponent();
            m_strTitle = this.Text;
            m_lstTasks = lstTasks;
            foreach (var task in m_lstTasks)
            {
                AddItem(task);
            }
        }
        private void AddItem(SQLite_Tasks objTask)
        {
            ListViewItem item = new ListViewItem();
            item.Text = objTask.id.ToString();
            item.SubItems.Add(objTask.name);
            item.SubItems.Add(objTask.value.ToString());
            lv_Data.Items.Add(item);
        }
        private void ChangeItem(SQLite_Tasks objTask)
        {
            for (int i = 0; i < lv_Data.Items.Count; i++)
            {
                if (lv_Data.Items[i].Text == objTask.id.ToString())
                {
                    lv_Data.Items[i].SubItems[1].Text = objTask.name;
                    lv_Data.Items[i].SubItems[2].Text = objTask.value.ToString();
                    break;
                }
            }
        }
        private void bt_Save_Click(object sender, EventArgs e)
        {
            if ("" == txt_TaskName.Text)
            {
                return;
            }
            int iValue = 0;
            try
            {
                iValue = int.Parse(txt_TaskValue.Text);
            }
            catch (Exception err)
            {
                MessageBox.Show(err.Message);
                return;
            }

            if (-1 == m_curId)
            {
                foreach (var task in m_lstTasks)
                {
                    if (task.name == txt_TaskName.Text
                        || task.value == iValue)
                    {
                        MessageBox.Show("任务名或值重复");
                        return;
                    }
                }
                var objTask = new SQLite_Tasks();
                objTask.name = txt_TaskName.Text;
                objTask.value = iValue;
                objTask.Save();
                m_lstTasks.Add(objTask);
                AddItem(objTask);
                bt_New_Click(sender, e);
            }
            else
            {
                foreach (var task in m_lstTasks)
                {
                    if ((task.name == txt_TaskName.Text
                        || task.value == iValue) && task.id != m_curId)
                    {
                        MessageBox.Show("任务名或值重复");
                        return;
                    }
                }
                for (int i = 0; i < m_lstTasks.Count; i++)
                {
                    if (m_curId == m_lstTasks[i].id)
                    {
                        var objTask = m_lstTasks[i];
                        objTask.name = txt_TaskName.Text;
                        objTask.value = int.Parse(txt_TaskValue.Text);
                        objTask.Save();
                        ChangeItem(objTask);
                        break;
                    }
                }
            }
        }
        private void DelListView(string strId)
        {
            for (int i = 0; i < lv_Data.Items.Count; i++)
            {
                if (lv_Data.Items[i].Text == strId)
                {
                    lv_Data.Items.RemoveAt(i);
                    break;
                }
            }
        }
        private void bt_Del_Click(object sender, EventArgs e)
        {
            for (int i = 0; i < m_lstTasks.Count; i++)
            {
                if (m_lstTasks[i].id == m_curId)
                {
                    DelListView(m_curId.ToString());
                    m_lstTasks[i].Delete();
                    m_lstTasks.RemoveAt(i);
                    bt_New_Click(sender, e);
                    break;
                }
            }
        }
        private void bt_New_Click(object sender, EventArgs e)
        {
            bt_Del.Enabled = false;
            m_curId = -1;
            txt_TaskName.Text = "";
            txt_TaskValue.Text = "";
            this.Text = m_strTitle;
            lv_Data.SelectedItems.Clear();
        }
        private void lv_Data_SelectedIndexChanged(object sender, EventArgs e)
        {
            if (0 == lv_Data.SelectedItems.Count)
            {
                bt_New_Click(sender, e);
            }
            else
            {
                bt_Del.Enabled = true;
                m_curId = int.Parse(lv_Data.SelectedItems[0].Text);
                txt_TaskName.Text = lv_Data.SelectedItems[0].SubItems[1].Text;
                txt_TaskValue.Text = lv_Data.SelectedItems[0].SubItems[2].Text;
                this.Text = m_strTitle + "  " + m_curId.ToString();
            }
        }
    }
}
