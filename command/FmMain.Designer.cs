namespace command
{
    partial class FmMain
    {
        /// <summary>
        /// 必需的设计器变量。
        /// </summary>
        private System.ComponentModel.IContainer components = null;

        /// <summary>
        /// 清理所有正在使用的资源。
        /// </summary>
        /// <param name="disposing">如果应释放托管资源，为 true；否则为 false。</param>
        protected override void Dispose(bool disposing)
        {
            if (disposing && (components != null))
            {
                components.Dispose();
            }
            base.Dispose(disposing);
        }

        #region Windows 窗体设计器生成的代码

        /// <summary>
        /// 设计器支持所需的方法 - 不要修改
        /// 使用代码编辑器修改此方法的内容。
        /// </summary>
        private void InitializeComponent()
        {
            System.ComponentModel.ComponentResourceManager resources = new System.ComponentModel.ComponentResourceManager(typeof(FmMain));
            this.splitContainer1 = new System.Windows.Forms.SplitContainer();
            this.tabControl1 = new System.Windows.Forms.TabControl();
            this.tabPage1 = new System.Windows.Forms.TabPage();
            this.tabPage2 = new System.Windows.Forms.TabPage();
            this.txt_Show = new System.Windows.Forms.TextBox();
            this.label1 = new System.Windows.Forms.Label();
            this.label2 = new System.Windows.Forms.Label();
            this.txt_HarborIp = new System.Windows.Forms.TextBox();
            this.txt_HarborPort = new System.Windows.Forms.TextBox();
            this.bt_HarborLink = new System.Windows.Forms.Button();
            this.label3 = new System.Windows.Forms.Label();
            this.cb_HarborName = new System.Windows.Forms.ComboBox();
            this.cb_HarborTask = new System.Windows.Forms.ComboBox();
            this.label4 = new System.Windows.Forms.Label();
            this.txt_HarborCMD = new System.Windows.Forms.TextBox();
            this.bt_HarborSave = new System.Windows.Forms.Button();
            this.bt_HarborRun = new System.Windows.Forms.Button();
            this.txt_HarborSearch = new System.Windows.Forms.TextBox();
            ((System.ComponentModel.ISupportInitialize)(this.splitContainer1)).BeginInit();
            this.splitContainer1.Panel1.SuspendLayout();
            this.splitContainer1.Panel2.SuspendLayout();
            this.splitContainer1.SuspendLayout();
            this.tabControl1.SuspendLayout();
            this.tabPage1.SuspendLayout();
            this.SuspendLayout();
            // 
            // splitContainer1
            // 
            this.splitContainer1.BorderStyle = System.Windows.Forms.BorderStyle.FixedSingle;
            this.splitContainer1.Dock = System.Windows.Forms.DockStyle.Fill;
            this.splitContainer1.Location = new System.Drawing.Point(0, 0);
            this.splitContainer1.Name = "splitContainer1";
            this.splitContainer1.Orientation = System.Windows.Forms.Orientation.Horizontal;
            // 
            // splitContainer1.Panel1
            // 
            this.splitContainer1.Panel1.Controls.Add(this.tabControl1);
            // 
            // splitContainer1.Panel2
            // 
            this.splitContainer1.Panel2.Controls.Add(this.txt_Show);
            this.splitContainer1.Size = new System.Drawing.Size(927, 727);
            this.splitContainer1.SplitterDistance = 429;
            this.splitContainer1.TabIndex = 0;
            // 
            // tabControl1
            // 
            this.tabControl1.Controls.Add(this.tabPage1);
            this.tabControl1.Controls.Add(this.tabPage2);
            this.tabControl1.Dock = System.Windows.Forms.DockStyle.Fill;
            this.tabControl1.Location = new System.Drawing.Point(0, 0);
            this.tabControl1.Name = "tabControl1";
            this.tabControl1.SelectedIndex = 0;
            this.tabControl1.Size = new System.Drawing.Size(925, 427);
            this.tabControl1.TabIndex = 1;
            // 
            // tabPage1
            // 
            this.tabPage1.Controls.Add(this.txt_HarborSearch);
            this.tabPage1.Controls.Add(this.bt_HarborRun);
            this.tabPage1.Controls.Add(this.bt_HarborSave);
            this.tabPage1.Controls.Add(this.txt_HarborCMD);
            this.tabPage1.Controls.Add(this.cb_HarborTask);
            this.tabPage1.Controls.Add(this.label4);
            this.tabPage1.Controls.Add(this.cb_HarborName);
            this.tabPage1.Controls.Add(this.label3);
            this.tabPage1.Controls.Add(this.bt_HarborLink);
            this.tabPage1.Controls.Add(this.txt_HarborPort);
            this.tabPage1.Controls.Add(this.txt_HarborIp);
            this.tabPage1.Controls.Add(this.label2);
            this.tabPage1.Controls.Add(this.label1);
            this.tabPage1.Location = new System.Drawing.Point(4, 26);
            this.tabPage1.Name = "tabPage1";
            this.tabPage1.Padding = new System.Windows.Forms.Padding(3);
            this.tabPage1.Size = new System.Drawing.Size(917, 397);
            this.tabPage1.TabIndex = 0;
            this.tabPage1.Text = "Harbor";
            this.tabPage1.UseVisualStyleBackColor = true;
            // 
            // tabPage2
            // 
            this.tabPage2.Location = new System.Drawing.Point(4, 26);
            this.tabPage2.Name = "tabPage2";
            this.tabPage2.Padding = new System.Windows.Forms.Padding(3);
            this.tabPage2.Size = new System.Drawing.Size(917, 333);
            this.tabPage2.TabIndex = 1;
            this.tabPage2.Text = "Echo";
            this.tabPage2.UseVisualStyleBackColor = true;
            // 
            // txt_Show
            // 
            this.txt_Show.Dock = System.Windows.Forms.DockStyle.Fill;
            this.txt_Show.Location = new System.Drawing.Point(0, 0);
            this.txt_Show.Multiline = true;
            this.txt_Show.Name = "txt_Show";
            this.txt_Show.ReadOnly = true;
            this.txt_Show.ScrollBars = System.Windows.Forms.ScrollBars.Both;
            this.txt_Show.Size = new System.Drawing.Size(925, 292);
            this.txt_Show.TabIndex = 0;
            // 
            // label1
            // 
            this.label1.AutoSize = true;
            this.label1.Location = new System.Drawing.Point(14, 13);
            this.label1.Name = "label1";
            this.label1.Size = new System.Drawing.Size(24, 16);
            this.label1.TabIndex = 0;
            this.label1.Text = "Ip";
            // 
            // label2
            // 
            this.label2.AutoSize = true;
            this.label2.Location = new System.Drawing.Point(189, 13);
            this.label2.Name = "label2";
            this.label2.Size = new System.Drawing.Size(40, 16);
            this.label2.TabIndex = 1;
            this.label2.Text = "Port";
            // 
            // txt_HarborIp
            // 
            this.txt_HarborIp.Location = new System.Drawing.Point(44, 8);
            this.txt_HarborIp.Name = "txt_HarborIp";
            this.txt_HarborIp.Size = new System.Drawing.Size(139, 26);
            this.txt_HarborIp.TabIndex = 2;
            // 
            // txt_HarborPort
            // 
            this.txt_HarborPort.Location = new System.Drawing.Point(232, 9);
            this.txt_HarborPort.Name = "txt_HarborPort";
            this.txt_HarborPort.Size = new System.Drawing.Size(79, 26);
            this.txt_HarborPort.TabIndex = 3;
            // 
            // bt_HarborLink
            // 
            this.bt_HarborLink.Location = new System.Drawing.Point(353, 6);
            this.bt_HarborLink.Name = "bt_HarborLink";
            this.bt_HarborLink.Size = new System.Drawing.Size(50, 28);
            this.bt_HarborLink.TabIndex = 4;
            this.bt_HarborLink.Text = "链接";
            this.bt_HarborLink.UseVisualStyleBackColor = true;
            this.bt_HarborLink.Click += new System.EventHandler(this.bt_HarborLink_Click);
            // 
            // label3
            // 
            this.label3.AutoSize = true;
            this.label3.Location = new System.Drawing.Point(186, 54);
            this.label3.Name = "label3";
            this.label3.Size = new System.Drawing.Size(40, 16);
            this.label3.TabIndex = 5;
            this.label3.Text = "名称";
            // 
            // cb_HarborName
            // 
            this.cb_HarborName.DropDownStyle = System.Windows.Forms.ComboBoxStyle.DropDownList;
            this.cb_HarborName.FormattingEnabled = true;
            this.cb_HarborName.Location = new System.Drawing.Point(231, 49);
            this.cb_HarborName.Name = "cb_HarborName";
            this.cb_HarborName.Size = new System.Drawing.Size(172, 24);
            this.cb_HarborName.TabIndex = 6;
            // 
            // cb_HarborTask
            // 
            this.cb_HarborTask.DropDownStyle = System.Windows.Forms.ComboBoxStyle.DropDownList;
            this.cb_HarborTask.FormattingEnabled = true;
            this.cb_HarborTask.Location = new System.Drawing.Point(46, 49);
            this.cb_HarborTask.Name = "cb_HarborTask";
            this.cb_HarborTask.Size = new System.Drawing.Size(134, 24);
            this.cb_HarborTask.TabIndex = 8;
            // 
            // label4
            // 
            this.label4.AutoSize = true;
            this.label4.Location = new System.Drawing.Point(4, 54);
            this.label4.Name = "label4";
            this.label4.Size = new System.Drawing.Size(40, 16);
            this.label4.TabIndex = 7;
            this.label4.Text = "任务";
            // 
            // txt_HarborCMD
            // 
            this.txt_HarborCMD.Anchor = ((System.Windows.Forms.AnchorStyles)((((System.Windows.Forms.AnchorStyles.Top | System.Windows.Forms.AnchorStyles.Bottom) 
            | System.Windows.Forms.AnchorStyles.Left) 
            | System.Windows.Forms.AnchorStyles.Right)));
            this.txt_HarborCMD.Location = new System.Drawing.Point(3, 84);
            this.txt_HarborCMD.Multiline = true;
            this.txt_HarborCMD.Name = "txt_HarborCMD";
            this.txt_HarborCMD.ScrollBars = System.Windows.Forms.ScrollBars.Both;
            this.txt_HarborCMD.Size = new System.Drawing.Size(914, 310);
            this.txt_HarborCMD.TabIndex = 9;
            // 
            // bt_HarborSave
            // 
            this.bt_HarborSave.Location = new System.Drawing.Point(647, 48);
            this.bt_HarborSave.Name = "bt_HarborSave";
            this.bt_HarborSave.Size = new System.Drawing.Size(50, 28);
            this.bt_HarborSave.TabIndex = 10;
            this.bt_HarborSave.Text = "保存";
            this.bt_HarborSave.UseVisualStyleBackColor = true;
            // 
            // bt_HarborRun
            // 
            this.bt_HarborRun.Location = new System.Drawing.Point(579, 48);
            this.bt_HarborRun.Name = "bt_HarborRun";
            this.bt_HarborRun.Size = new System.Drawing.Size(50, 28);
            this.bt_HarborRun.TabIndex = 11;
            this.bt_HarborRun.Text = "执行";
            this.bt_HarborRun.UseVisualStyleBackColor = true;
            // 
            // txt_HarborSearch
            // 
            this.txt_HarborSearch.Location = new System.Drawing.Point(409, 48);
            this.txt_HarborSearch.Name = "txt_HarborSearch";
            this.txt_HarborSearch.Size = new System.Drawing.Size(145, 26);
            this.txt_HarborSearch.TabIndex = 12;
            // 
            // FmMain
            // 
            this.AutoScaleDimensions = new System.Drawing.SizeF(8F, 16F);
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Font;
            this.ClientSize = new System.Drawing.Size(927, 727);
            this.Controls.Add(this.splitContainer1);
            this.Font = new System.Drawing.Font("宋体", 12F);
            this.Icon = ((System.Drawing.Icon)(resources.GetObject("$this.Icon")));
            this.Margin = new System.Windows.Forms.Padding(4, 4, 4, 4);
            this.MaximizeBox = false;
            this.Name = "FmMain";
            this.StartPosition = System.Windows.Forms.FormStartPosition.CenterScreen;
            this.Text = "命令";
            this.FormClosed += new System.Windows.Forms.FormClosedEventHandler(this.FmMain_FormClosed);
            this.splitContainer1.Panel1.ResumeLayout(false);
            this.splitContainer1.Panel2.ResumeLayout(false);
            this.splitContainer1.Panel2.PerformLayout();
            ((System.ComponentModel.ISupportInitialize)(this.splitContainer1)).EndInit();
            this.splitContainer1.ResumeLayout(false);
            this.tabControl1.ResumeLayout(false);
            this.tabPage1.ResumeLayout(false);
            this.tabPage1.PerformLayout();
            this.ResumeLayout(false);

        }

        #endregion

        private System.Windows.Forms.SplitContainer splitContainer1;
        private System.Windows.Forms.TabControl tabControl1;
        private System.Windows.Forms.TabPage tabPage1;
        private System.Windows.Forms.TabPage tabPage2;
        private System.Windows.Forms.TextBox txt_Show;
        private System.Windows.Forms.TextBox txt_HarborPort;
        private System.Windows.Forms.TextBox txt_HarborIp;
        private System.Windows.Forms.Label label2;
        private System.Windows.Forms.Label label1;
        private System.Windows.Forms.Button bt_HarborLink;
        private System.Windows.Forms.ComboBox cb_HarborName;
        private System.Windows.Forms.Label label3;
        private System.Windows.Forms.ComboBox cb_HarborTask;
        private System.Windows.Forms.Label label4;
        private System.Windows.Forms.Button bt_HarborRun;
        private System.Windows.Forms.Button bt_HarborSave;
        private System.Windows.Forms.TextBox txt_HarborCMD;
        private System.Windows.Forms.TextBox txt_HarborSearch;
    }
}

